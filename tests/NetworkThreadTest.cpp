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

// The network stacks with real threads (tests/HostThreads.cpp, checked with ThreadSanitizer): two machines, each a NetworkStackSet with its own
// service thread, joined by a wire whose interfaces "interrupt" when a frame arrives. Application threads use their sockets the way programs
// do, waiting with wait_for_progress() instead of polling: several connections echo at once from several threads; a ping-pong over the
// loopback is woken by the stack itself (not the service thread's 5 ms idle timer); and an application's send() gets in while frames pour in.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
#include "Network/NetworkStackSet.hpp"

using namespace network;

static std::atomic<int> g_failed{0};
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct SafeAllocator final : allocators::IAllocator {
  std::atomic<long> live{0};
  void* allocate(uint64_t size) override { if (!size) return nullptr; ++live; return aligned_alloc(64, (size + 63) / 64 * 64); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    if (!size) return nullptr;
    ++live;
    if (alignment < 64) alignment = 64;
    return aligned_alloc(alignment, (size + alignment - 1) / alignment * alignment);
  }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};
struct RealClock final : hardware::IClock {
  const char* name() const override { return "steady clock"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint64_t now_us() const override {
    return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
  }
};

// One end of the wire: frames another thread sends arrive here; an arrival "interrupts" (wakes the service thread of this end's stack).
struct WireNic final : hardware::INetworkInterface {
  WireNic* peer = nullptr;
  hardware::MacAddress mac;
  std::mutex mutex;
  std::deque<std::vector<uint8_t>> inbox;
  hardware::NetworkWake wake = nullptr;
  void* wake_context = nullptr;
  std::atomic<uint64_t> sent{0};
  explicit WireNic(uint8_t last) { mac = {{0x02, 0, 0, 0, 0, last}}; }
  const char* name() const override { return "wire nic"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  hardware::MacAddress mac_address() const override { return mac; }
  bool link_up() const override { return true; }
  bool enable_interrupts(hardware::NetworkWake w, void* context) override {
    std::lock_guard<std::mutex> guard(mutex);
    wake = w;
    wake_context = context;
    return true;
  }
  void disable_interrupts() override {
    std::lock_guard<std::mutex> guard(mutex);
    wake = nullptr;
  }
  bool interrupts_enabled() const override { return true; }
  void arrive(ConstByteView frame) {
    hardware::NetworkWake w;
    void* c;
    {
      std::lock_guard<std::mutex> guard(mutex);
      if (inbox.size() > 1024) return;   // (a full ring drops)
      inbox.emplace_back(frame.data(), frame.data() + frame.size());
      if (inbox.back().size() < 60) inbox.back().resize(60);
      w = wake;
      c = wake_context;
    }
    if (w) w(c);
  }
  bool transmit(ConstByteView frame) override {
    ++sent;
    peer->arrive(frame);
    return true;
  }
  bool receive(ByteView buffer, uint32_t* length) override {
    std::lock_guard<std::mutex> guard(mutex);
    if (inbox.empty()) return false;
    const std::vector<uint8_t>& f = inbox.front();
    const uint32_t n = f.size() < buffer.size() ? uint32_t(f.size()) : uint32_t(buffer.size());
    for (uint32_t i = 0; i < n; ++i) buffer[i] = f[i];
    *length = uint32_t(f.size());
    inbox.pop_front();
    return true;
  }
  hardware::NetworkStatistics statistics() const override { return hardware::NetworkStatistics{}; }
};

struct Machines {
  SafeAllocator alloc;
  RealClock clock;
  WireNic nic_a{1}, nic_b{2};
  NetworkStackSet a{clock, alloc}, b{clock, alloc};
  Machines() {
    nic_a.peer = &nic_b;
    nic_b.peer = &nic_a;
    CHECK(a.add(nic_a) && b.add(nic_b));
    IpConfiguration ca, cb;
    ca.address = Ipv4Address::make(10, 0, 0, 1);
    ca.netmask = cb.netmask = Ipv4Address::make(255, 255, 255, 0);
    cb.address = Ipv4Address::make(10, 0, 0, 2);
    a.at(0)->configure(ca);
    b.at(0)->configure(cb);
    CHECK(a.start_service() && b.start_service());
  }
  ~Machines() {
    a.stop_service();
    b.stop_service();
  }
};

// Waits (as a program's thread would: woken by the stacks) until `done` or `seconds` pass.
template <typename Done>
static bool wait_until(NetworkStackSet& set, Done&& done, double seconds = 20) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
  for (;;) {
    const uint64_t seen = set.progress();
    if (done()) return true;
    if (std::chrono::steady_clock::now() > end) return false;
    set.wait_for_progress(seen, 2000);
  }
}

// Several connections at once, each with a thread on either side: every byte comes back, in order.
static void test_concurrent_connections() {
  Machines m;
  constexpr int kConnections = 6;
  constexpr uint32_t kBytes = 96 * 1024;
  std::vector<std::thread> threads;
  std::atomic<int> echoed_ok{0}, servers_done{0};
  for (int c = 0; c < kConnections; ++c) {
    const uint16_t port = uint16_t(9000 + c);
    SharedPointer<ITcpSocket> listener = m.b.at(0)->tcp_open();
    CHECK(listener->listen(port));
    threads.emplace_back([&m, listener, &servers_done] {   // the echo server: everything back, until the client closes
      SharedPointer<ITcpSocket> conn;
      CHECK(wait_until(m.b, [&] { return bool(conn = listener->accept()); }));
      if (!conn) return;
      uint8_t buffer[4096];
      std::vector<uint8_t> pending;
      wait_until(m.b, [&] {
        for (uint32_t n; (n = conn->receive(ByteView(buffer))) > 0;) pending.insert(pending.end(), buffer, buffer + n);
        while (!pending.empty()) {
          const uint32_t k = conn->send(ConstByteView(pending.data(), pending.size()));
          if (!k) break;
          pending.erase(pending.begin(), pending.begin() + k);
        }
        return pending.empty() && conn->peer_closed();
      }, 60);
      conn->close();
      ++servers_done;
    });
    threads.emplace_back([&m, port, c, &echoed_ok] {   // the client
      SharedPointer<ITcpSocket> s = m.a.at(0)->tcp_open();
      CHECK(s->connect(Ipv4Address::make(10, 0, 0, 2), port));
      CHECK(wait_until(m.a, [&] { return s->is_connected() || s->failed(); }));
      std::vector<uint8_t> out(kBytes), in;
      for (uint32_t i = 0; i < kBytes; ++i) out[i] = uint8_t(i * 7 + c);
      uint32_t offset = 0;
      uint8_t buffer[4096];
      wait_until(m.a, [&] {
        if (offset < kBytes) offset += s->send(ConstByteView(out.data() + offset, kBytes - offset));
        for (uint32_t n; (n = s->receive(ByteView(buffer))) > 0;) in.insert(in.end(), buffer, buffer + n);
        return in.size() >= kBytes || s->failed();
      }, 60);
      if (in == out) ++echoed_ok;
      s->close();
    });
  }
  for (std::thread& t : threads) t.join();
  printf("concurrent: %d of %d connections echoed %u bytes each\n", echoed_ok.load(), kConnections, kBytes);
  CHECK(echoed_ok == kConnections && servers_done == kConnections);
}

// A ping-pong between two threads over the loopback: each message is delivered by a poll the sender's send() asked for (the stack's wake hook)
// and the other thread is woken by the poll's progress - so a round trip takes microseconds, not the service thread's 5 ms idle period.
static void test_loopback_latency() {
  Machines m;
  NetworkStack& stack = *m.a.at(0);
  SharedPointer<ITcpSocket> listener = stack.tcp_open();
  CHECK(listener->listen(9500));
  constexpr int kRounds = 300;
  std::atomic<bool> server_ok{false};
  std::thread server([&] {
    SharedPointer<ITcpSocket> conn;
    CHECK(wait_until(m.a, [&] { return bool(conn = listener->accept()); }));
    if (!conn) return;
    uint8_t b[8];
    int rounds = 0;
    wait_until(m.a, [&] {
      while (conn->available() >= 8 && rounds < kRounds) {
        conn->receive(ByteView(b));
        conn->send(ConstByteView(b));
        ++rounds;
      }
      return rounds == kRounds;
    }, 60);
    server_ok = rounds == kRounds;
  });
  SharedPointer<ITcpSocket> client = stack.tcp_open();
  CHECK(client->connect(Ipv4Address::make(127, 0, 0, 1), 9500));
  CHECK(wait_until(m.a, [&] { return client->is_connected(); }));
  const auto start = std::chrono::steady_clock::now();
  int done = 0;
  for (; done < kRounds; ++done) {
    uint8_t q[8] = {uint8_t(done)};
    if (client->send(ConstByteView(q)) != 8) break;
    if (!wait_until(m.a, [&] { return client->available() >= 8; }, 5)) break;
    client->receive(ByteView(q));
  }
  const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
  server.join();
  printf("loopback: %d round trips, %.0f us each on average\n", done, done ? us / done : 0.0);
  CHECK(done == kRounds && server_ok);
  CHECK(us / kRounds < 2500);   // (without the wake hook a round trip waits for the 5 ms idle timer twice: 10 ms; even ThreadSanitizer's slowdown stays well under)
}

// A flood of frames into machine A (raw UDP datagrams put on its wire as fast as a thread can) keeps its service thread busy; an application
// thread's send() still gets the stack's lock between batches, so its datagrams go out while the flood lasts (it would wait for whole polls,
// back to back, otherwise).
static void test_send_during_flood() {
  Machines m;
  SharedPointer<IUdpSocket> heard = m.b.at(0)->udp_open(9800);
  SharedPointer<IUdpSocket> talker = m.a.at(0)->udp_open(0);
  // (a datagram from 10.0.0.2:1234 to 10.0.0.1:9700, where nothing listens; the UDP checksum 0 means "none")
  std::vector<uint8_t> frame(14 + 20 + 8 + 512, 0);
  for (int i = 0; i < 6; ++i) { frame[i] = m.nic_a.mac.bytes[i]; frame[6 + i] = m.nic_b.mac.bytes[i]; }
  frame[12] = 0x08; frame[14] = 0x45; frame[16] = uint8_t((20 + 8 + 512) >> 8); frame[17] = uint8_t(20 + 8 + 512); frame[22] = 64; frame[23] = 17;
  frame[26] = 10; frame[29] = 2; frame[30] = 10; frame[33] = 1;
  uint32_t sum = 0;
  for (int i = 14; i < 34; i += 2) sum += uint32_t(frame[i] << 8 | frame[i + 1]);
  while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
  frame[24] = uint8_t(~sum >> 8); frame[25] = uint8_t(~sum);
  frame[34] = 1234 >> 8; frame[35] = 1234 & 0xFF; frame[36] = 9700 >> 8; frame[37] = 9700 & 0xFF; frame[38] = (8 + 512) >> 8; frame[39] = uint8_t(8 + 512);
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> flooded{0};
  std::thread flooder([&] {
    while (!stop) {
      m.nic_a.arrive(ConstByteView(frame.data(), frame.size()));
      ++flooded;
    }
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  int got = 0;
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < 200; ++i) {
    const uint8_t msg[4] = {uint8_t(i)};
    talker->send_to(Ipv4Address::make(10, 0, 0, 2), 9800, ConstByteView(msg));
    uint8_t d[16];
    Ipv4Address from;
    uint16_t port;
    uint32_t n;
    if (wait_until(m.b, [&] { return heard->receive_from(ByteView(d), &from, &port, &n); }, 2)) ++got;
  }
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  stop = true;
  flooder.join();
  printf("during a flood of %llu frames: %d of 200 datagrams sent and heard, %.3f ms each\n", (unsigned long long)flooded.load(), got, ms / 200);
  CHECK(got >= 190 && ms / 200 < 5);   // (without the hand-over between batches: 17 ms)
}

int main() {
  test_concurrent_connections();
  test_loopback_latency();
  test_send_during_flood();
  if (g_failed) { printf("NetworkThreadTest: %d failed\n", g_failed.load()); return 1; }
  printf("NetworkThreadTest: all passed\n");
  return 0;
}
