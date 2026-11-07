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
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>
#include "Kernel/Interrupts.hpp"
#include "Network/NetworkStackSet.hpp"
#include "Network/Tcp/TcpSocket.hpp"
#include "Stream/Tcp/TcpStream.hpp"

using namespace network;
using namespace stream;

// (the executor's fatal errors: HostThreads.cpp has no kernel panic)
namespace kernel {
[[noreturn]] void panic(const char* what, const Frame*) {
  fprintf(stderr, "PANIC: %s\n", what);
  abort();
}
}  // namespace kernel

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

// One end of the wire: frames another thread sends arrive here; an arrival "interrupts" (wakes the service threads of this end's stack). With
// `queues` 2 it is an interface with RSS: a TCP frame goes to the queue its ports pick (so a flow stays in one), anything else to queue 0.
struct WireNic final : hardware::INetworkInterface {
  static constexpr uint32_t kMaxQueues = 2;
  WireNic* peer = nullptr;
  hardware::MacAddress mac;
  uint32_t queues = 1;
  std::mutex mutex[kMaxQueues];
  std::deque<std::vector<uint8_t>> inbox[kMaxQueues];
  std::mutex wake_mutex;
  hardware::NetworkWake wake = nullptr;
  void* wake_context = nullptr;
  std::atomic<uint64_t> sent{0}, received_on[kMaxQueues] = {{0}, {0}};
  explicit WireNic(uint8_t last, uint32_t queue_count = 1) : queues(queue_count) { mac = {{0x02, 0, 0, 0, 0, last}}; }
  const char* name() const override { return "wire nic"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  hardware::MacAddress mac_address() const override { return mac; }
  bool link_up() const override { return true; }
  bool enable_interrupts(hardware::NetworkWake w, void* context) override {
    std::lock_guard<std::mutex> guard(wake_mutex);
    wake = w;
    wake_context = context;
    return true;
  }
  void disable_interrupts() override {
    std::lock_guard<std::mutex> guard(wake_mutex);
    wake = nullptr;
  }
  bool interrupts_enabled() const override { return true; }
  uint32_t queue_of(ConstByteView frame) const {
    if (queues == 1 || frame.size() < 38 || frame[12] != 0x08 || frame[13] != 0x00 || frame[23] != 6) return 0;
    const uint32_t ports = uint32_t(frame[34] << 8 | frame[35]) + uint32_t(frame[36] << 8 | frame[37]);
    return ports % queues;
  }
  void arrive(ConstByteView frame) {
    const uint32_t q = queue_of(frame);
    {
      std::lock_guard<std::mutex> guard(mutex[q]);
      if (inbox[q].size() > 1024) return;   // (a full ring drops)
      inbox[q].emplace_back(frame.data(), frame.data() + frame.size());
      if (inbox[q].back().size() < 60) inbox[q].back().resize(60);
    }
    hardware::NetworkWake w;
    void* c;
    {
      std::lock_guard<std::mutex> guard(wake_mutex);
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
  uint32_t receive_queue_count() const override { return queues; }
  size_t waiting(uint32_t q) {
    std::lock_guard<std::mutex> guard(mutex[q]);
    return inbox[q].size();
  }
  bool receive_frame_from(uint32_t q, ByteView buffer, hardware::ReceivedFrame* frame) override {
    *frame = hardware::ReceivedFrame{};
    if (q >= queues) return false;
    std::lock_guard<std::mutex> guard(mutex[q]);
    if (inbox[q].empty()) return false;
    const std::vector<uint8_t>& f = inbox[q].front();
    const uint32_t n = f.size() < buffer.size() ? uint32_t(f.size()) : uint32_t(buffer.size());
    for (uint32_t i = 0; i < n; ++i) buffer[i] = f[i];
    frame->length = uint32_t(f.size());
    inbox[q].pop_front();
    ++received_on[q];
    return true;
  }
  bool receive(ByteView buffer, uint32_t* length) override {
    hardware::ReceivedFrame frame;
    for (uint32_t q = 0; q < queues; ++q)
      if (receive_frame_from(q, buffer, &frame)) {
        *length = frame.length;
        return true;
      }
    return false;
  }
  hardware::NetworkStatistics statistics() const override { return hardware::NetworkStatistics{}; }
};

struct Machines {
  SafeAllocator alloc;
  RealClock clock;
  WireNic nic_a, nic_b;
  NetworkStackSet a{clock, alloc}, b{clock, alloc};
  explicit Machines(uint32_t queues = 1) : nic_a(1, queues), nic_b(2, queues) {
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

// A flood of frames into machine A (raw UDP datagrams put on its wire as fast as its ring takes them) keeps its service thread busy; an
// application thread's send() still gets the locks it needs (the IP lock, a fair one, for a moment), so its datagrams go out at once while the
// flood lasts - and all of them arrive. (Before receiving took no stack-wide lock and the locks were fair, a send waited for whole polls:
// 17 ms.)
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
      if (m.nic_a.waiting(0) >= 1024) {   // (the ring is full: a real wire's frames would be dropped; this thread need not spin on it)
        std::this_thread::yield();
        continue;
      }
      m.nic_a.arrive(ConstByteView(frame.data(), frame.size()));
      ++flooded;
    }
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  int got = 0;
  double send_us = 0, slowest_send_us = 0;
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < 200; ++i) {
    const uint8_t msg[4] = {uint8_t(i)};
    const auto before = std::chrono::steady_clock::now();
    talker->send_to(Ipv4Address::make(10, 0, 0, 2), 9800, ConstByteView(msg));
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - before).count();
    send_us += us;
    if (us > slowest_send_us) slowest_send_us = us;
    uint8_t d[16];
    Ipv4Address from;
    uint16_t port;
    uint32_t n;
    if (wait_until(m.b, [&] { return heard->receive_from(ByteView(d), &from, &port, &n); }, 2)) ++got;
  }
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  stop = true;
  flooder.join();
  printf("during a flood of %llu frames: %d of 200 datagrams sent and heard, send() %.0f us on average (slowest %.0f), %.3f ms each end to end\n",
         (unsigned long long)flooded.load(), got, send_us / 200, slowest_send_us, ms / 200);
  CHECK(got >= 190 && send_us / 200 < 500);
}

// The stack's lock is a fair one (kernel::Mutex(true)). A thread that holds it in long stretches and takes it again the moment it lets go (as
// the polling thread does) and one that needs it only briefly: with the fair lock they take strict turns. (In the kernel an ordinary one lets
// the first take it straight back: the second is woken only to find it held again.)
static void test_fair_lock() {
  double ratios[2] = {0, 0};
  for (int fair = 0; fair < 2; ++fair) {
    kernel::RecursiveMutex lock(fair != 0);
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> hog{0}, brief{0};
    std::thread holder([&] {
      while (!stop) {
        kernel::ScopedRecursiveMutex guard(lock);
        kernel::ScopedRecursiveMutex again(lock);   // (recursive)
        ++hog;
        for (volatile int spin = 0; spin < 20000; ++spin) {}
      }
    });
    std::thread other([&] {
      while (!stop) {
        kernel::ScopedRecursiveMutex guard(lock);
        ++brief;
      }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    stop = true;
    holder.join();
    other.join();
    ratios[fair] = double(brief) / double(hog ? hog.load() : 1);
    printf("%s lock: the long holder %llu times, the brief one %llu (%.2f per turn of the other)\n", fair ? "fair" : "plain", (unsigned long long)hog.load(),
           (unsigned long long)brief.load(), ratios[fair]);
  }
  CHECK(ratios[1] > 0.95 && ratios[1] < 1.05);   // (strictly in turn: the host's plain lock, which naps between tries, is merely random)
}

// An async task (an Executor running TcpStream) in ping-pong with a server thread over the loopback, its executor woken by the stack's progress
// (TcpStream::idle_wake): every round trip completes, and quickly.
static async::Task<void> ping(network::INetworkStack& stack, allocators::IAllocator& allocator, int rounds, int* done) {
  UniquePointer<IAsyncStream> stream;
  if (co_await TcpStream::connect_async(stack, allocator, IpAddress(Ipv4Address::make(127, 0, 0, 1)), 9600, 5000, &stream) != StreamStatus::Ok) co_return;
  for (int i = 0; i < rounds; ++i) {
    uint8_t q[8] = {uint8_t(i)};
    if (co_await stream->write_async(ConstByteView(q)) != StreamStatus::Ok) co_return;
    uint32_t got = 0;
    while (got < 8) {
      const IoResult r = co_await stream->read_async(ByteView(q).subview(got));
      if (r.status != StreamStatus::Ok || r.count == 0) co_return;
      got += r.count;
    }
    ++*done;
  }
}

static void test_executor_wake() {
  Machines m;
  async::set_frame_allocator(m.alloc);
  NetworkStack& stack = *m.a.at(0);
  SharedPointer<ITcpSocket> listener = stack.tcp_open();
  CHECK(listener->listen(9600));
  constexpr int kRounds = 200;
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
  });
  async::Executor executor(m.clock);
  executor.set_idle_wake(TcpStream::idle_wake(stack));
  int done = 0;
  const auto start = std::chrono::steady_clock::now();
  executor.block_on(ping(stack, m.alloc, kRounds, &done));
  const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
  server.join();
  printf("executor: %d async round trips, %.0f us each on average\n", done, done ? us / done : 0.0);
  CHECK(done == kRounds && us / kRounds < 2500);
}

// Two bulk connections into machine B, whose interface has two receive queues (one flow on each): B runs a service thread per queue, and the
// two flows' segments are handled at the same moment - each under its own socket's lock, neither queue holding a stack-wide lock - while
// every byte arrives intact and in order.
static void test_parallel_queues() {
  Machines m(2);
  CHECK(m.b.at(0)->receive_queues() == 2);
  constexpr int kFlows = 2;
  constexpr uint32_t kBytes = 512 * 1024;
  std::atomic<int> intact{0};
  std::vector<std::thread> threads;
  for (int f = 0; f < kFlows; ++f) {
    const uint16_t port = uint16_t(9100 + 2 * f);   // (client ports 49152 and 49153, in either order: the port sums differ by an odd number, one per queue)
    SharedPointer<ITcpSocket> listener = m.b.at(0)->tcp_open();
    CHECK(listener->listen(port));
    threads.emplace_back([&m, listener, f, &intact] {   // the receiver: checks the pattern as it reads
      SharedPointer<ITcpSocket> conn;
      CHECK(wait_until(m.b, [&] { return bool(conn = listener->accept()); }));
      if (!conn) return;
      uint32_t got = 0;
      bool same = true;
      uint8_t buffer[4096];
      wait_until(m.b, [&] {
        for (uint32_t n; (n = conn->receive(ByteView(buffer))) > 0; got += n)
          for (uint32_t i = 0; i < n; ++i) same = same && buffer[i] == uint8_t((got + i) * 13 + f);
        return got >= kBytes || conn->failed();
      }, 120);
      if (same && got == kBytes) ++intact;
    });
    threads.emplace_back([&m, port, f] {   // the sender
      SharedPointer<ITcpSocket> s = m.a.at(0)->tcp_open();
      CHECK(s->connect(Ipv4Address::make(10, 0, 0, 2), port));
      CHECK(wait_until(m.a, [&] { return s->is_connected() || s->failed(); }));
      std::vector<uint8_t> out(kBytes);
      for (uint32_t i = 0; i < kBytes; ++i) out[i] = uint8_t(i * 13 + f);
      uint32_t offset = 0;
      wait_until(m.a, [&] {
        if (offset < kBytes) offset += s->send(ConstByteView(out.data() + offset, kBytes - offset));
        return (offset == kBytes && s->unsent_or_unacknowledged() == 0) || s->failed();
      }, 120);
      s->close();
    });
  }
  for (std::thread& t : threads) t.join();
  printf("parallel queues: %d of %d flows intact, %llu and %llu frames on B's two queues, at most %u queues handled at once\n", intact.load(), kFlows,
         (unsigned long long)m.nic_b.received_on[0].load(), (unsigned long long)m.nic_b.received_on[1].load(), m.b.at(0)->concurrent_receives_seen());
  CHECK(intact == kFlows);
  CHECK(m.nic_b.received_on[0] > 100 && m.nic_b.received_on[1] > 100);   // (the flows really went to different queues)
  CHECK(m.b.at(0)->concurrent_receives_seen() == 2);
}

// Many connections at once (the lookup tables are hashed and read with RCU): four client threads open 64 connections each to one listener,
// whose server thread accepts and answers them all, while the receive path looks every segment's connection up without a lock and the
// threads that connect and close change the table under it. Every answer is right, and the table empties again once they are all closed.
static void test_many_connections() {
  Machines m(2);
  constexpr int kThreads = 4, kPerThread = 64, kTotal = kThreads * kPerThread;
  SharedPointer<ITcpSocket> listener = m.b.at(0)->tcp_open();
  CHECK(listener->listen(9200));
  std::atomic<int> answered{0}, clients_done{0};
  std::thread server([&] {   // accepts everything, answers each 4-byte question with the bytes reversed, closes after its client does
    std::vector<SharedPointer<ITcpSocket>> conns;
    std::vector<std::vector<uint8_t>> got;
    wait_until(m.b, [&] {
      while (SharedPointer<ITcpSocket> c = listener->accept()) {
        conns.push_back(c);
        got.emplace_back();
      }
      for (size_t i = 0; i < conns.size(); ++i) {
        if (!conns[i]) continue;
        uint8_t buffer[16];
        for (uint32_t n; (n = conns[i]->receive(ByteView(buffer))) > 0;) got[i].insert(got[i].end(), buffer, buffer + n);
        if (got[i].size() == 4) {
          const uint8_t answer[4] = {got[i][3], got[i][2], got[i][1], got[i][0]};
          if (conns[i]->send(ConstByteView(answer, 4)) == 4) got[i].push_back(0);   // (answered)
        }
        if (conns[i]->peer_closed()) {
          conns[i]->close();
          conns[i] = SharedPointer<ITcpSocket>();
        }
      }
      return clients_done == kThreads && std::all_of(conns.begin(), conns.end(), [](const SharedPointer<ITcpSocket>& c) { return !c; }) &&
             conns.size() == size_t(kTotal);
    }, 120);
  });
  std::vector<std::thread> clients;
  for (int t = 0; t < kThreads; ++t)
    clients.emplace_back([&, t] {
      std::vector<SharedPointer<ITcpSocket>> socks;
      for (int i = 0; i < kPerThread; ++i) {
        SharedPointer<ITcpSocket> s = m.a.at(0)->tcp_open();
        CHECK(s->connect(Ipv4Address::make(10, 0, 0, 2), 9200));
        socks.push_back(s);
      }
      std::vector<bool> asked(kPerThread, false), done(kPerThread, false);
      std::vector<std::vector<uint8_t>> in(kPerThread);
      wait_until(m.a, [&] {
        bool all = true;
        for (int i = 0; i < kPerThread; ++i) {
          if (done[i]) continue;
          all = false;
          const uint8_t question[4] = {uint8_t(t), uint8_t(i), uint8_t(i * 3), 0x5A};
          if (!asked[i] && socks[i]->is_connected()) asked[i] = socks[i]->send(ConstByteView(question, 4)) == 4;
          uint8_t buffer[16];
          for (uint32_t n; (n = socks[i]->receive(ByteView(buffer))) > 0;) in[i].insert(in[i].end(), buffer, buffer + n);
          if (in[i].size() >= 4 || socks[i]->failed()) {
            if (in[i].size() == 4 && in[i][0] == 0x5A && in[i][1] == uint8_t(i * 3) && in[i][2] == uint8_t(i) && in[i][3] == uint8_t(t)) ++answered;
            socks[i]->close();
            done[i] = true;
          }
        }
        return all;
      }, 120);
      ++clients_done;
    });
  for (std::thread& c : clients) c.join();
  server.join();
  const bool emptied = wait_until(m.a, [&] { return m.a.at(0)->tcp_connection_count() == 0 && m.b.at(0)->tcp_connection_count() == 0; }, 30);
  printf("many connections: %d of %d answered, tables emptied: %s\n", answered.load(), kTotal, emptied ? "yes" : "no");
  CHECK(answered == kTotal);
  CHECK(emptied);
}

// One connection, several threads: on each machine one thread sends while another receives on the same socket, both directions at once, while
// the service threads process its segments. Nobody waits for the protocol work of another (send() and receive() take only their buffer's
// lock; segments that find the connection busy go to its backlog), and every byte arrives intact and in order.
static void test_one_connection_threads() {
  Machines m(2);
  constexpr uint32_t kBytes = 1024 * 1024;
  SharedPointer<ITcpSocket> listener = m.b.at(0)->tcp_open();
  CHECK(listener->listen(9300));
  SharedPointer<ITcpSocket> client = m.a.at(0)->tcp_open();
  CHECK(client->connect(Ipv4Address::make(10, 0, 0, 2), 9300));
  SharedPointer<ITcpSocket> server;
  CHECK(wait_until(m.b, [&] { return bool(server = listener->accept()); }));
  CHECK(wait_until(m.a, [&] { return client->is_connected(); }));
  if (!server || !client->is_connected()) return;
  std::atomic<int> intact{0};
  const auto sender = [&](NetworkStackSet& set, SharedPointer<ITcpSocket> s, uint8_t seed) {
    std::vector<uint8_t> out(kBytes);
    for (uint32_t i = 0; i < kBytes; ++i) out[i] = uint8_t(i * 11 + seed);
    uint32_t offset = 0;
    wait_until(set, [&] {
      while (offset < kBytes) {
        const uint32_t k = s->send(ConstByteView(out.data() + offset, kBytes - offset));
        if (!k) break;
        offset += k;
      }
      return (offset == kBytes && s->unsent_or_unacknowledged() == 0) || s->failed();
    }, 120);
  };
  const auto receiver = [&](NetworkStackSet& set, SharedPointer<ITcpSocket> s, uint8_t seed) {
    uint32_t got = 0;
    bool same = true;
    uint8_t buffer[3000];
    wait_until(set, [&] {
      for (uint32_t n; (n = s->receive(ByteView(buffer))) > 0; got += n)
        for (uint32_t i = 0; i < n; ++i) same = same && buffer[i] == uint8_t((got + i) * 11 + seed);
      return got >= kBytes || s->failed();
    }, 120);
    if (same && got == kBytes) ++intact;
  };
  std::thread a_send(sender, std::ref(m.a), client, 1), a_receive(receiver, std::ref(m.a), client, 2);
  std::thread b_send(sender, std::ref(m.b), server, 2), b_receive(receiver, std::ref(m.b), server, 1);
  a_send.join();
  a_receive.join();
  b_send.join();
  b_receive.join();
  auto* a = static_cast<TcpSocket*>(client.get());
  auto* b = static_cast<TcpSocket*>(server.get());
  printf("one connection, four threads: %d of 2 directions intact, %llu + %llu segments through the backlog, %llu dropped\n", intact.load(),
         (unsigned long long)(a->backlogged_segments()), (unsigned long long)(b->backlogged_segments()),
         (unsigned long long)(a->backlog_drops() + b->backlog_drops()));
  CHECK(intact == 2);
  client->close();
  server->close();
}

// The backlog, deterministically: a thread holds the server's connection (as an application's call or a timer would) while the client sends.
// The service thread does not wait for it - the segments go to the backlog and another connection on the same machine is served meanwhile -
// and a send() on the held connection returns at once; when the holder lets go it processes the backlog itself, and the data is there.
static void test_backlog() {
  Machines m;
  SharedPointer<ITcpSocket> listener = m.b.at(0)->tcp_open();
  CHECK(listener->listen(9400));
  SharedPointer<ITcpSocket> c1 = m.a.at(0)->tcp_open(), c2 = m.a.at(0)->tcp_open();
  CHECK(c1->connect(Ipv4Address::make(10, 0, 0, 2), 9400) && c2->connect(Ipv4Address::make(10, 0, 0, 2), 9400));
  SharedPointer<ITcpSocket> s1, s2;
  CHECK(wait_until(m.b, [&] {
    if (!s1) s1 = listener->accept();
    else if (!s2) s2 = listener->accept();
    return bool(s2);
  }));
  CHECK(wait_until(m.a, [&] { return c1->is_connected() && c2->is_connected(); }));
  if (!s1 || !s2) return;
  // (which accepted connection is c1's: the one whose peer port is c1's)
  if (s1->remote_port() != c1->local_port()) {
    SharedPointer<ITcpSocket> t = s1;
    s1 = s2;
    s2 = t;
  }
  auto* held = static_cast<TcpSocket*>(s1.get());
  constexpr uint32_t kBytes = 8000;
  std::vector<uint8_t> out(kBytes);
  for (uint32_t i = 0; i < kBytes; ++i) out[i] = uint8_t(i * 5 + 3);
  bool other_served = false, send_returned = false;
  uint64_t backlogged = 0;
  uint32_t visible_while_held = 0;
  {
    std::optional<TcpSocket::Owner> hold;
    hold.emplace(*held);
    const uint64_t before = held->backlogged_segments();
    CHECK(c1->send(ConstByteView(out.data(), kBytes)) == kBytes);
    wait_until(m.b, [&] { return held->backlogged_segments() - before >= 3; }, 10);
    backlogged = held->backlogged_segments() - before;
    visible_while_held = s1->available();   // (nothing processed: it waits in the backlog)
    // the service thread is not stuck behind the held connection: the other one is served
    const uint8_t ping[3] = {1, 2, 3};
    CHECK(c2->send(ConstByteView(ping, 3)) == 3);
    other_served = wait_until(m.b, [&] { return s2->available() == 3; }, 10);
    // nor is an application's send() on the held connection (its segments go out when the holder lets go)
    std::atomic<bool> returned{false};
    std::thread t([&] {
      const uint8_t reply[2] = {9, 8};
      CHECK(s1->send(ConstByteView(reply, 2)) == 2);
      returned = true;
    });
    for (int i = 0; i < 2000 && !returned; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    send_returned = returned;
    hold.reset();   // (letting go processes the backlog on this thread, and sends the reply; before the join, so a send() that waits fails, not hangs)
    t.join();
  }
  const uint32_t visible_after = s1->available();
  std::vector<uint8_t> in(kBytes + 16);
  uint32_t got = 0;
  wait_until(m.b, [&] {
    for (uint32_t n; (n = s1->receive(ByteView(in.data() + got, in.size() - got))) > 0;) got += n;
    return got >= kBytes;
  }, 10);
  uint8_t reply[4];
  uint32_t replied = 0;
  wait_until(m.a, [&] { return (replied += c1->receive(ByteView(reply + replied, sizeof reply - replied))) >= 2; }, 10);
  printf("backlog: %llu segments kept while held (%u bytes visible then, %u right after), other connection served: %s, send returned: %s\n",
         (unsigned long long)backlogged, visible_while_held, visible_after, other_served ? "yes" : "no", send_returned ? "yes" : "no");
  CHECK(backlogged >= 3 && visible_while_held == 0 && visible_after > 0);
  CHECK(other_served && send_returned);
  CHECK(got == kBytes && std::equal(out.begin(), out.end(), in.begin()));
  CHECK(replied == 2 && reply[0] == 9 && reply[1] == 8);
}

// One UDP socket, several threads: on machine A four threads send datagrams from the same socket while a fifth reads the echoes from it; on B
// one thread echoes everything back on its socket while the service threads (two receive queues) deliver into both. The socket's only lock is
// its queue's, held for a copy, so a delivery never waits for a send. Every echo that comes back is intact (UDP may drop: the senders keep
// at most 16 datagrams outstanding, below the queue's 32, and nearly all must return).
static void test_udp_threads() {
  Machines m(2);
  constexpr int kSenders = 4;
  constexpr uint32_t kPerSender = 400, kSize = 600;
  SharedPointer<IUdpSocket> echo = m.b.at(0)->udp_open(9500);
  SharedPointer<IUdpSocket> shared = m.a.at(0)->udp_open(0);
  CHECK(echo && shared);
  if (!echo || !shared) return;
  std::atomic<bool> stop{false};
  std::atomic<uint32_t> sent{0}, echoed{0}, corrupt{0};
  std::thread echoer([&] {
    uint8_t buffer[2000];
    while (!stop) {
      IpAddress from;
      uint16_t port;
      uint32_t length;
      const uint64_t seen = m.b.progress();
      bool any = false;
      while (echo->receive_from_ip(ByteView(buffer), &from, &port, &length)) {
        echo->send_to_ip(from, port, ConstByteView(buffer, length));
        any = true;
      }
      if (!any) m.b.wait_for_progress(seen, 5);
    }
  });
  std::thread reader([&] {
    uint8_t buffer[2000];
    while (!stop) {
      uint32_t length;
      const uint64_t seen = m.a.progress();
      bool any = false;
      while (shared->receive_from_ip(ByteView(buffer), nullptr, nullptr, &length)) {
        any = true;
        bool good = length == kSize;
        for (uint32_t i = 4; good && i < kSize; ++i) good = buffer[i] == uint8_t(buffer[0] * 31 + buffer[1] + buffer[2] * 7 + i);
        if (good) ++echoed;
        else ++corrupt;
      }
      if (!any) m.a.wait_for_progress(seen, 5);
    }
  });
  std::vector<std::thread> senders;
  for (int t = 0; t < kSenders; ++t)
    senders.emplace_back([&, t] {
      uint8_t d[kSize];
      for (uint32_t n = 0; n < kPerSender; ++n) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (sent - echoed - corrupt >= 16 && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        d[0] = uint8_t(t);
        d[1] = uint8_t(n);
        d[2] = uint8_t(n >> 8);
        d[3] = 0;
        for (uint32_t i = 4; i < kSize; ++i) d[i] = uint8_t(d[0] * 31 + d[1] + d[2] * 7 + i);
        if (shared->send_to(Ipv4Address::make(10, 0, 0, 2), 9500, ConstByteView(d, kSize))) ++sent;
      }
    });
  for (std::thread& t : senders) t.join();
  wait_until(m.a, [&] { return echoed + corrupt >= sent; }, 10);
  stop = true;
  echoer.join();
  reader.join();
  printf("udp threads: %u sent from %d threads on one socket, %u echoed intact, %u corrupt\n", sent.load(), kSenders, echoed.load(), corrupt.load());
  CHECK(sent == kSenders * kPerSender);
  CHECK(corrupt == 0 && echoed >= sent * 9 / 10);
}

int main() {
  test_concurrent_connections();
  test_loopback_latency();
  test_send_during_flood();
  test_fair_lock();
  test_executor_wake();
  test_parallel_queues();
  test_many_connections();
  test_one_connection_threads();
  test_backlog();
  test_udp_threads();
  if (g_failed) { printf("NetworkThreadTest: %d failed\n", g_failed.load()); return 1; }
  printf("NetworkThreadTest: all passed\n");
  return 0;
}
