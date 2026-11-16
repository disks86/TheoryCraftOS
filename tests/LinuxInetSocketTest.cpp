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
// Host tests of AF_INET and AF_INET6 sockets for Linux programs (src/Process/LinuxInetSockets.cpp over src/Process/Sockets.cpp): one simulated machine (lan.a)
// is "this process", making the Linux system calls a C library makes; the other side of the wire (lan.b) is driven through the network stack's own API, as a
// peer somewhere else. A call that waits sleeps through the fake host, which steps the simulated wire and runs the test's "what the peer does meanwhile".
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include "NetworkHarness.hpp"
#include "Ipv6Harness.hpp"
#include "FileSystem/Ram/RamFileSystem.hpp"
#include "Process/Process.hpp"
#include "Process/Sockets.hpp"
#include "Process/Syscall.hpp"
#include "Process/UnixSockets.hpp"

using namespace process;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

constexpr uint64_t kBase = 0x100000, kSize = 0x40000, kBad = 0xDEAD0000;
struct FakeMemory final : IUserMemory {
  uint8_t bytes[kSize] = {};
  bool read(uint64_t a, ByteView out) override {
    if (a < kBase || a - kBase > sizeof(bytes) || out.size() > sizeof(bytes) - (a - kBase)) return false;
    for (uint64_t i = 0; i < out.size(); ++i) out[i] = bytes[a - kBase + i];
    return true;
  }
  bool write(uint64_t a, ConstByteView in) override {
    if (a < kBase || a - kBase > sizeof(bytes) || in.size() > sizeof(bytes) - (a - kBase)) return false;
    for (uint64_t i = 0; i < in.size(); ++i) bytes[a - kBase + i] = in[i];
    return true;
  }
};
struct Host final : IProcessHost {
  Lan* lan = nullptr;
  std::function<void()> meanwhile;      // what the peer does while this process waits (there is no second thread in a host test)
  int steps = 0;
  bool write_console(uint32_t, ConstByteView) override { return true; }
  bool read_console(ByteView, uint64_t* got) override { *got = 0; return true; }
  uint64_t realtime_ns() override { return 0; }
  uint64_t monotonic_ns() override { return lan ? lan->clock.us * 1000ull : 0; }
  void sleep_ns(uint64_t ns) override {
    if (++steps > 400000) { printf("test stuck waiting (no progress after many steps)\n"); exit(1); }
    if (lan) lan->step(int(ns / 1000000) + 1);
    if (meanwhile) meanwhile();
  }
  void yield() override { sleep_ns(0); }
};
struct Runtime final : IProcessRuntime {
  Host* host = nullptr;
  void lock() override {}
  void unlock() override {}
  bool wait(uint64_t ns) override { host->sleep_ns(ns > 2000000 ? 2000000 : ns); return true; }
  void notify() override {}
  bool interrupted() override { return false; }
  bool start_thread(const ThreadStart&, bool) override { return false; }
  bool resume_thread(uint64_t) override { return false; }
  uint64_t current_thread_id() override { return 7; }
};
// The system's lock for the descriptor manager (poll and epoll wait on it): a wait lets the network go on.
struct LanSync final : ISystemSync {
  Host* host = nullptr;
  void lock() override {}
  void unlock() override {}
  bool wait(uint64_t ns) override { host->sleep_ns(ns > 2000000 ? 2000000 : ns); return true; }
  void notify_all() override {}
};
struct OneStack final : network::IStackProvider {
  network::INetworkStack* stack;
  explicit OneStack(network::INetworkStack* s) : stack(s) {}
  uint32_t count() const override { return 1; }
  network::INetworkStack* at(uint32_t index) const override { return index == 0 ? stack : nullptr; }
};

constexpr uint64_t kAfInet = 2, kAfInet6 = 10, kStream = 1, kDgram = 2, kNonBlock = 0x800, kCloexec = 0x80000;
constexpr uint16_t kPollIn = 1, kPollOut = 4, kPollErr = 8, kPollHup = 0x10;

struct Setup {
  Lan lan;
  OneStack provider{&lan.a};
  SocketManager sockets{provider};
  FakeMemory memory;
  Host host;
  Runtime runtime;
  LanSync sync;
  UnixSocketManager descriptors{sync, lan.alloc};
  Process process{7, ProcessBinding{SyscallAbi::Linux, UserArch::X86_64, syscall_interface(SyscallAbi::Linux, UserArch::X86_64)}, memory, host};
  uint64_t cursor = kBase + 0x1000;
  Setup() {
    lan.configure_static();
    set_system_sockets(&sockets);
    set_system_unix_sockets(&descriptors);
    host.lan = &lan;
    runtime.host = &host;
    sync.host = &host;
    process.attach_runtime(&runtime);
  }
  ~Setup() {
    process.release_handles();
    set_system_unix_sockets(nullptr);
    set_system_sockets(nullptr);
  }
  void configure_ipv6() {
    lan.run_until([&] { return lan.a.configuration6().link_local_ready && lan.b.configuration6().link_local_ready; }, 3000);
    lan.nic_a.inbox.push_back(router_advertisement(v6("2001:db8:1::"), 1800));
    lan.nic_b.inbox.push_back(router_advertisement(v6("2001:db8:1::"), 1800));
    lan.run_until([&] { return lan.a.configuration6().global_ready && lan.b.configuration6().global_ready; }, 3000);
  }

  int64_t s(uint64_t n, uint64_t a0 = 0, uint64_t a1 = 0, uint64_t a2 = 0, uint64_t a3 = 0, uint64_t a4 = 0, uint64_t a5 = 0) {
    SyscallRequest r;
    r.number = n;
    r.args[0] = a0; r.args[1] = a1; r.args[2] = a2; r.args[3] = a3; r.args[4] = a4; r.args[5] = a5;
    return int64_t(process.syscall(r));
  }
  uint64_t alloc(uint64_t n) { const uint64_t at = cursor; cursor += (n + 15) & ~uint64_t{15}; if (cursor > kBase + kSize) { printf("test memory used up\n"); exit(1); } return at; }
  void reuse() { cursor = kBase + 0x1000; }      // (the memory handed out so far is used again)
  uint64_t bytes(const std::string& text) {
    const uint64_t at = alloc(text.size() + 1);
    memory.write(at, ConstByteView(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
    return at;
  }
  std::string text(uint64_t at, uint64_t n) { std::string out(n, 0); memory.read(at, ByteView(reinterpret_cast<uint8_t*>(&out[0]), n)); return out; }
  void put32(uint64_t at, uint32_t v) { uint8_t b[4]; for (int i = 0; i < 4; ++i) b[i] = uint8_t(v >> (8 * i)); memory.write(at, ConstByteView(b, 4)); }
  void put64(uint64_t at, uint64_t v) { uint8_t b[8]; for (int i = 0; i < 8; ++i) b[i] = uint8_t(v >> (8 * i)); memory.write(at, ConstByteView(b, 8)); }
  uint32_t u32(uint64_t at) { uint8_t b[4] = {}; memory.read(at, ByteView(b, 4)); return b[0] | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16 | uint32_t(b[3]) << 24; }
  uint64_t u64(uint64_t at) { return u32(at) | uint64_t(u32(at + 4)) << 32; }
  // struct sockaddr_in / sockaddr_in6 in the program's memory
  uint64_t in4(uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint16_t port) {
    const uint8_t raw[16] = {2, 0, uint8_t(port >> 8), uint8_t(port), a, b, c, d};
    const uint64_t at = alloc(16);
    memory.write(at, ConstByteView(raw, 16));
    return at;
  }
  uint64_t in6(const network::Ipv6Address& address, uint16_t port) {
    uint8_t raw[28] = {10, 0, uint8_t(port >> 8), uint8_t(port)};
    for (int i = 0; i < 16; ++i) raw[8 + i] = address.bytes[i];
    const uint64_t at = alloc(28);
    memory.write(at, ConstByteView(raw, 28));
    return at;
  }
  uint64_t word(uint32_t v) { const uint64_t at = alloc(8); put32(at, v); return at; }
  // poll() of one descriptor without waiting: its revents
  int polled(int64_t fd, uint16_t events = kPollIn | kPollOut) {
    const uint64_t at = alloc(8);
    put32(at, uint32_t(fd)); put32(at + 4, events);
    return s(7, at, 1, 0) < 0 ? -1 : int(u32(at + 4) >> 16);
  }
  // getsockopt of an int
  int64_t option(int64_t fd, uint64_t level, uint64_t name, uint32_t* value) {
    const uint64_t at = alloc(8), size = word(4);
    put32(at, 0xEEEEEEEE);
    const int64_t r = s(55, fd, level, name, at, size);
    *value = u32(at);
    return r;
  }
};

static std::string received_by(SharedPointer<network::ITcpSocket>& peer) {
  std::string out;
  uint8_t b[2048];
  for (uint32_t n; (n = peer->receive(ByteView(b))) > 0;) out.append(reinterpret_cast<char*>(b), n);
  return out;
}
static void send_from(SharedPointer<network::ITcpSocket>& peer, const std::string& text) {
  ConstByteView v(reinterpret_cast<const uint8_t*>(text.data()), text.size());
  while (!v.empty()) v = v.subview(peer->send(v));
}

// A program connects to a server elsewhere: connect, write, read, the names of both ends, the end of the stream.
static void test_tcp_client() {
  Setup t;
  SharedPointer<network::ITcpSocket> listener = t.lan.b.tcp_open(), peer;
  CHECK(listener->listen(9000));
  t.host.meanwhile = [&] { if (!peer) peer = listener->accept(); };
  const int64_t fd = t.s(41, kAfInet, kStream | kCloexec, 0);
  CHECK(fd >= 3 && t.process.files().close_on_exec(uint64_t(fd)) && t.process.files().kind(uint64_t(fd)) == DescriptorKind::Socket);
  uint32_t v = 0;
  CHECK(t.option(fd, 1, 3, &v) == 0 && v == kStream && t.option(fd, 1, 39, &v) == 0 && v == kAfInet && t.option(fd, 1, 38, &v) == 0 && v == 6 && t.option(fd, 1, 30, &v) == 0 && v == 0);
  CHECK(t.s(1, fd, t.bytes("x"), 1) == -32 && t.s(0, fd, t.alloc(8), 8) == -107 && t.s(52, fd, t.alloc(16), t.word(16)) == -107);      // ENOTCONN before it is connected
  CHECK(t.s(42, fd, t.in4(10, 0, 0, 2, 9000), 16) == 0 && peer && peer->is_connected());
  CHECK(t.s(42, fd, t.in4(10, 0, 0, 2, 9000), 16) == -106);      // EISCONN
  CHECK(t.s(1, fd, t.bytes("hello from the client"), 21) == 21);
  CHECK(t.lan.run_until([&] { return peer->available() >= 21; }, 2000) && received_by(peer) == "hello from the client");
  // the answer: a read that waits for it, then one that takes what is there, then (MSG_WAITALL) one that waits for all it asked for
  bool sent = false;
  t.host.meanwhile = [&] { if (!sent) { sent = true; send_from(peer, "hello from the server"); } };
  const uint64_t buf = t.alloc(256);
  CHECK(t.s(0, fd, buf, 5) == 5 && t.text(buf, 5) == "hello");
  CHECK(t.lan.run_until([&] { return t.sockets.readable(uint32_t(t.process.files().token(uint64_t(fd)))) >= 16; }, 2000));
  CHECK(t.polled(fd) == (kPollIn | kPollOut) && t.s(45, fd, buf, 256, 0x40 /* MSG_DONTWAIT */, 0, 0) == 16 && t.text(buf, 16) == " from the server");
  CHECK(t.polled(fd) == kPollOut && t.s(45, fd, buf, 256, 0x40, 0, 0) == -11);
  int pieces = 0;
  t.host.meanwhile = [&] { if (pieces < 3 && t.host.steps % 5 == 0) { ++pieces; send_from(peer, "abcd"); } };
  CHECK(t.s(45, fd, buf, 12, 0x100 /* MSG_WAITALL */, 0, 0) == 12 && t.text(buf, 12) == "abcdabcdabcd");
  t.host.meanwhile = nullptr;
  // who is at each end
  const uint64_t name = t.alloc(32), size = t.word(28);
  CHECK(t.s(51, fd, name, size) == 0 && t.u32(size) == 16 && t.text(name, 2) == std::string("\x02\0", 2) && t.text(name + 4, 4) == std::string("\x0a\0\0\x01", 4));
  CHECK((uint8_t(t.text(name + 2, 1)[0]) << 8 | uint8_t(t.text(name + 3, 1)[0])) == peer->remote_port());
  t.put32(size, 28);
  CHECK(t.s(52, fd, name, size) == 0 && t.u32(size) == 16 && t.text(name + 2, 6) == std::string("\x23\x28\x0a\0\0\x02", 6));      // port 9000, 10.0.0.2
  t.put32(size, 5);      // a buffer too small takes the first bytes, and the whole size is reported
  memset(&t.memory.bytes[name - kBase], 0xEE, 16);
  CHECK(t.s(52, fd, name, size) == 0 && t.u32(size) == 16 && uint8_t(t.text(name + 4, 1)[0]) == 0x0a && uint8_t(t.text(name + 5, 1)[0]) == 0xEE);
  CHECK(t.s(52, fd, kBad, size) == -14 && t.s(52, fd, name, kBad) == -14 && t.s(51, fd, 0, size) == -14);
  // recvfrom of a stream leaves the address alone; sendto's address is not looked at
  t.put32(size, 99);
  sent = false;
  t.host.meanwhile = [&] { if (!sent) { sent = true; send_from(peer, "more"); } };
  CHECK(t.s(45, fd, buf, 64, 0, name, size) == 4 && t.u32(size) == 99 && t.s(44, fd, t.bytes("again"), 5, 0, t.in4(1, 2, 3, 4, 5), 16) == 5);
  t.host.meanwhile = nullptr;
  // the server closes: the end of the stream is a read of 0, and poll says there is something to read
  peer->close();
  CHECK(t.s(0, fd, buf, 64) == 0 && (t.polled(fd) & kPollIn) != 0 && t.s(0, fd, buf, 64) == 0);
  CHECK(t.s(48, fd, 1) == 0 && t.s(1, fd, buf, 1) == -32 && t.s(48, fd, 3) == -22);      // shutdown(SHUT_WR): EPIPE afterwards
  CHECK(t.s(3, fd) == 0 && t.s(0, fd, buf, 1) == -9 && t.sockets.state(1) == SocketManager::State::None);
}

// A program that has said all it has to say (shutdown(SHUT_WR)) still reads what the other end sends afterwards, and then the end of the stream.
static void test_half_close() {
  Setup t;
  SharedPointer<network::ITcpSocket> listener = t.lan.b.tcp_open(), peer;
  CHECK(listener->listen(9050));
  t.host.meanwhile = [&] { if (!peer) peer = listener->accept(); };
  const int64_t fd = t.s(41, kAfInet, kStream, 0);
  CHECK(t.s(42, fd, t.in4(10, 0, 0, 2, 9050), 16) == 0 && t.s(1, fd, t.bytes("request"), 7) == 7 && t.s(48, fd, 1) == 0);
  CHECK(t.lan.run_until([&] { return peer && peer->available() >= 7; }, 3000) && received_by(peer) == "request");
  CHECK(t.lan.run_until([&] { return peer->peer_closed(); }, 3000));      // the other end sees the end of what this one sends
  CHECK(t.s(1, fd, t.bytes("x"), 1) == -32);      // EPIPE: its own side is closed
  bool sent = false;
  t.host.meanwhile = [&] { if (!sent) { sent = true; send_from(peer, "late answer"); peer->close(); } };
  const uint64_t buf = t.alloc(64);
  CHECK(t.s(0, fd, buf, 64) == 11 && t.text(buf, 11) == "late answer" && t.s(0, fd, buf, 64) == 0);
  uint32_t v = 9;
  CHECK(t.option(fd, 1, 4, &v) == 0 && v == 0 && (t.polled(fd) & kPollHup) != 0);      // no error: it ended in good order, in both directions
  t.host.meanwhile = nullptr;
}

// A connect that does not wait, one that nobody answers, and what poll and SO_ERROR say about them.
static void test_connecting() {
  Setup t;
  SharedPointer<network::ITcpSocket> listener = t.lan.b.tcp_open();
  CHECK(listener->listen(9001));
  const int64_t fd = t.s(41, kAfInet, kStream | kNonBlock, 6);
  CHECK(fd >= 3 && (t.s(72, fd, 3) & 0x800) != 0 && t.polled(fd) == (kPollOut | kPollHup));      // (not connected: writable and hung up, as on Linux)
  CHECK(t.s(42, fd, t.in4(10, 0, 0, 2, 9001), 16) == -115);      // EINPROGRESS
  CHECK(t.s(42, fd, t.in4(10, 0, 0, 2, 9001), 16) == -114 && t.polled(fd) == 0);      // EALREADY; neither readable nor writable yet
  CHECK(t.s(1, fd, t.bytes("x"), 1) == -11);
  // poll waits for it
  const uint64_t pfd = t.alloc(8);
  t.put32(pfd, uint32_t(fd)); t.put32(pfd + 4, kPollOut);
  CHECK(t.s(7, pfd, 1, 3000) == 1 && (t.u32(pfd + 4) >> 16) == kPollOut);
  uint32_t v = 9;
  CHECK(t.option(fd, 1, 4, &v) == 0 && v == 0 && t.s(42, fd, t.in4(10, 0, 0, 2, 9001), 16) == -106 && t.s(1, fd, t.bytes("x"), 1) == 1);
  // nobody listens there: refused, at once for a socket that waits, through poll and SO_ERROR for one that does not
  const int64_t waits = t.s(41, kAfInet, kStream, 0);
  CHECK(t.s(42, waits, t.in4(10, 0, 0, 2, 9099), 16) == -111 && t.s(42, waits, t.in4(10, 0, 0, 2, 9099), 16) == -111);
  const int64_t later = t.s(41, kAfInet, kStream | kNonBlock, 0);
  CHECK(t.s(42, later, t.in4(10, 0, 0, 2, 9099), 16) == -115);
  t.put32(pfd, uint32_t(later)); t.put32(pfd + 4, kPollIn | kPollOut);
  CHECK(t.s(7, pfd, 1, 3000) == 1 && ((t.u32(pfd + 4) >> 16) & (kPollErr | kPollHup)) == (kPollErr | kPollHup));
  CHECK(t.option(later, 1, 4, &v) == 0 && v == 111 && t.s(1, later, t.bytes("x"), 1) == -111 && t.s(0, later, t.alloc(8), 8) == -111);
  // FIONBIO and O_NONBLOCK are one flag
  CHECK(t.s(16, fd, 0x5421, t.word(0)) == 0 && (t.s(72, fd, 3) & 0x800) == 0 && t.s(72, fd, 4, 0x800) == 0 && (t.s(72, fd, 3) & 0x800) != 0 && (t.s(72, fd, 3) & 3) == 2);
  CHECK(t.s(16, fd, 0x5421, kBad) == -14 && t.s(16, fd, 0x1234, 0) == -25);
}

// A program that listens: bind, listen, accept (waiting, not waiting, through poll), and the connection it gets.
static void test_tcp_server() {
  Setup t;
  const int64_t listener = t.s(41, kAfInet, kStream, 0);
  uint32_t v = 0;
  CHECK(t.s(54, listener, 1, 2, t.word(1), 4) == 0 && t.option(listener, 1, 2, &v) == 0 && v == 1);      // SO_REUSEADDR
  CHECK(t.s(49, listener, t.in4(0, 0, 0, 0, 9100), 16) == 0 && t.s(49, listener, t.in4(0, 0, 0, 0, 9101), 16) == -22);      // bound already
  CHECK(t.s(43, listener, 0, 0) == -22);      // accept before listen
  CHECK(t.s(50, listener, 16) == 0 && t.s(50, listener, 16) == 0 && t.option(listener, 1, 30, &v) == 0 && v == 1);
  const uint64_t name = t.alloc(32), size = t.word(16);
  CHECK(t.s(51, listener, name, size) == 0 && t.text(name + 2, 2) == std::string("\x23\x8c", 2));      // port 9100
  CHECK(t.s(72, listener, 4, 0x800) == 0 && t.s(43, listener, 0, 0) == -11 && t.polled(listener, kPollIn) == 0);
  SharedPointer<network::ITcpSocket> client = t.lan.b.tcp_open();
  CHECK(client->connect_ip(network::IpAddress(network::Ipv4Address::make(10, 0, 0, 1)), 9100));
  const uint64_t pfd = t.alloc(8);
  t.put32(pfd, uint32_t(listener)); t.put32(pfd + 4, kPollIn);
  CHECK(t.s(7, pfd, 1, 3000) == 1 && (t.u32(pfd + 4) >> 16) == kPollIn);
  t.put32(size, 16);
  const int64_t fd = t.s(288, listener, name, size, kNonBlock | kCloexec);
  CHECK(fd > listener && t.u32(size) == 16 && t.text(name, 2) == std::string("\x02\0", 2) && t.text(name + 4, 4) == std::string("\x0a\0\0\x02", 4));
  CHECK((t.s(72, fd, 3) & 0x800) != 0 && t.process.files().close_on_exec(uint64_t(fd)) && t.s(43, listener, 0, 0) == -11);
  CHECK(t.lan.run_until([&] { return client->is_connected(); }, 2000));
  send_from(client, "to the server");
  const uint64_t buf = t.alloc(64);
  t.put32(pfd, uint32_t(fd));
  CHECK(t.s(7, pfd, 1, 3000) == 1 && t.s(0, fd, buf, 64) == 13 && t.text(buf, 13) == "to the server" && t.s(0, fd, buf, 64) == -11);
  CHECK(t.s(1, fd, t.bytes("and back"), 8) == 8 && t.lan.run_until([&] { return client->available() >= 8; }, 2000) && received_by(client) == "and back");
  // a second connection, accepted by a call that waits for it; a copy of a descriptor keeps the socket when the other is closed
  CHECK(t.s(72, listener, 4, 0) == 0);
  SharedPointer<network::ITcpSocket> second = t.lan.b.tcp_open();
  bool started = false;
  t.host.meanwhile = [&] { if (!started) { started = true; second->connect_ip(network::IpAddress(network::Ipv4Address::make(10, 0, 0, 1)), 9100); } };
  const int64_t other = t.s(43, listener, 0, 0);
  t.host.meanwhile = nullptr;
  CHECK(other > fd);
  const int64_t copy = t.s(32, other);
  CHECK(copy > other && t.s(3, other) == 0 && t.s(1, copy, t.bytes("still here"), 10) == 10);
  CHECK(t.lan.run_until([&] { return second->available() >= 10; }, 2000) && received_by(second) == "still here");
  // fstat: a socket
  const uint64_t st = t.alloc(160);
  CHECK(t.s(5, copy, st) == 0 && (t.u32(st + 24) & 0170000) == 0140000);
  CHECK(t.s(288, listener, 0, 0, 1) == -22 && t.s(43, copy, 0, 0) == -22 && t.s(50, copy, 1) == -22);
  CHECK(t.s(288, listener, name, kBad, 0) == -14);
  // another socket cannot listen on the same port
  const int64_t clash = t.s(41, kAfInet, kStream, 0);
  CHECK(t.s(49, clash, t.in4(0, 0, 0, 0, 9100), 16) == 0 && t.s(50, clash, 1) == -98);      // EADDRINUSE
}

// Datagrams: to an address, from an address, to and from a remembered peer.
static void test_udp() {
  Setup t;
  SharedPointer<network::IUdpSocket> peer = t.lan.b.udp_open(9200);
  CHECK(peer);
  const int64_t fd = t.s(41, kAfInet, kDgram, 17);
  uint32_t v = 0;
  CHECK(fd >= 3 && t.option(fd, 1, 3, &v) == 0 && v == kDgram && t.polled(fd) == kPollOut);
  CHECK(t.s(1, fd, t.bytes("x"), 1) == -89);      // EDESTADDRREQ: nobody to send it to
  CHECK(t.s(44, fd, t.bytes("to the peer"), 11, 0, t.in4(10, 0, 0, 2, 9200), 16) == 11);
  uint8_t buffer[64] = {};
  network::IpAddress from;
  uint16_t from_port = 0;
  uint32_t length = 0;
  CHECK(t.lan.run_until([&] { return peer->pending() > 0; }, 2000) && peer->receive_from_ip(ByteView(buffer, sizeof(buffer)), &from, &from_port, &length) && length == 11);
  CHECK(std::string(reinterpret_cast<char*>(buffer), 11) == "to the peer");
  // the answer comes with its sender's address; a datagram longer than the buffer is cut
  bool sent = false;
  t.host.meanwhile = [&] { if (!sent) { sent = true; peer->send_to_ip(from, from_port, ConstByteView(reinterpret_cast<const uint8_t*>("an answer"), 9)); } };
  const uint64_t buf = t.alloc(64), name = t.alloc(32), size = t.word(28);
  CHECK(t.s(45, fd, buf, 64, 0, name, size) == 9 && t.text(buf, 9) == "an answer" && t.u32(size) == 16 && t.text(name + 2, 6) == std::string("\x23\xf0\x0a\0\0\x02", 6));
  t.host.meanwhile = nullptr;
  CHECK(peer->send_to_ip(from, from_port, ConstByteView(reinterpret_cast<const uint8_t*>("0123456789"), 10)) && t.lan.run_until([&] { return t.polled(fd, kPollIn) == kPollIn; }, 2000));
  CHECK(t.s(16, fd, 0x541B, t.word(0)) == 0 && t.s(45, fd, buf, 4, 0, 0, 0) == 4 && t.text(buf, 4) == "0123" && t.s(45, fd, buf, 64, 0x40, 0, 0) == -11);
  // connect() remembers the peer: write and read without addresses, getpeername
  CHECK(t.s(42, fd, t.in4(10, 0, 0, 2, 9200), 16) == 0 && t.s(1, fd, t.bytes("plain write"), 11) == 11);
  CHECK(t.lan.run_until([&] { return peer->pending() > 0; }, 2000) && peer->receive_from_ip(ByteView(buffer, sizeof(buffer)), &from, &from_port, &length) && length == 11);
  t.put32(size, 28);
  CHECK(t.s(52, fd, name, size) == 0 && t.text(name + 2, 6) == std::string("\x23\xf0\x0a\0\0\x02", 6));
  // sendmsg / recvmsg: several buffers, one datagram
  const uint64_t a = t.bytes("one-"), b = t.bytes("two-"), c = t.bytes("three");
  const uint64_t vectors = t.alloc(48), message = t.alloc(56);
  t.put64(vectors, a); t.put64(vectors + 8, 4); t.put64(vectors + 16, b); t.put64(vectors + 24, 4); t.put64(vectors + 32, c); t.put64(vectors + 40, 5);
  t.put64(message, 0); t.put64(message + 8, 0); t.put64(message + 16, vectors); t.put64(message + 24, 3); t.put64(message + 32, 0); t.put64(message + 40, 0); t.put64(message + 48, 0);
  CHECK(t.s(46, fd, message, 0) == 13 && t.lan.run_until([&] { return peer->pending() > 0; }, 2000));
  CHECK(peer->receive_from_ip(ByteView(buffer, sizeof(buffer)), &from, &from_port, &length) && length == 13 && std::string(reinterpret_cast<char*>(buffer), 13) == "one-two-three");
  CHECK(t.s(20, fd, vectors, 2) == 8 && t.lan.run_until([&] { return peer->pending() > 0; }, 2000) && peer->receive_from_ip(ByteView(buffer, sizeof(buffer)), &from, &from_port, &length) && length == 8);
  CHECK(peer->send_to_ip(from, from_port, ConstByteView(reinterpret_cast<const uint8_t*>("ABCDEFGHIJ"), 10)));
  const uint64_t r1 = t.alloc(8), r2 = t.alloc(8);
  t.put64(vectors, r1); t.put64(vectors + 8, 3); t.put64(vectors + 16, r2); t.put64(vectors + 24, 4);
  t.put64(message, name); t.put32(message + 8, 28); t.put64(message + 24, 2); t.put64(message + 40, 77); t.put32(message + 48, 0xEE);
  CHECK(t.s(47, fd, message, 0) == 7 && t.text(r1, 3) == "ABC" && t.text(r2, 4) == "DEFG" && t.u32(message + 8) == 16 && t.u64(message + 40) == 0 && t.u32(message + 48) == 0);
  // what a datagram cannot be; what a socket of this kind cannot do
  const uint64_t big = t.alloc(70000);
  CHECK(t.s(44, fd, big, 1473, 0, 0, 0) == -90 && t.s(44, fd, big, 70000, 0, 0, 0) == -90 && t.s(44, fd, big, 1472, 0, 0, 0) == 1472);      // EMSGSIZE: no fragments
  CHECK(t.s(50, fd, 1) == -95 && t.s(43, fd, 0, 0) == -95);
  CHECK(t.s(44, fd, kBad, 4, 0, 0, 0) == -14 && t.s(45, fd, kBad, 4, 0x40, 0, 0) == -11 && t.s(46, fd, kBad, 0) == -14 && t.s(47, fd, kBad, 0) == -14);
  // a time to wait at most (SO_RCVTIMEO): EAGAIN when it is over
  t.put64(buf, 0); t.put64(buf + 8, 300000);
  CHECK(t.s(54, fd, 1, 20, buf, 16) == 0);
  const uint64_t before = t.host.monotonic_ns();
  CHECK(t.s(45, fd, buf, 64, 0, 0, 0) == -11 && t.host.monotonic_ns() - before >= 300000000ull && t.host.monotonic_ns() - before < 400000000ull);
  const uint64_t timeval = t.alloc(16), tsize = t.word(16);
  CHECK(t.s(55, fd, 1, 20, timeval, tsize) == 0 && t.u64(timeval) == 0 && t.u64(timeval + 8) == 300000 && t.u32(tsize) == 16);
  // a bound socket receives what is sent to its port
  const int64_t bound = t.s(41, kAfInet, kDgram | kNonBlock, 0);
  CHECK(t.s(49, bound, t.in4(0, 0, 0, 0, 9300), 16) == 0 && t.s(45, bound, buf, 64, 0, 0, 0) == -11);
  CHECK(peer->send_to_ip(network::IpAddress(network::Ipv4Address::make(10, 0, 0, 1)), 9300, ConstByteView(reinterpret_cast<const uint8_t*>("knock"), 5)));
  CHECK(t.lan.run_until([&] { return t.polled(bound, kPollIn) == kPollIn; }, 2000) && t.s(45, bound, buf, 64, 0, 0, 0) == 5 && t.text(buf, 5) == "knock");
}

// Both ends in one program, over the loopback: no frame goes on the wire. Also epoll, and large amounts of data in both directions.
static void test_loopback_and_epoll() {
  Setup t;
  const int64_t listener = t.s(41, kAfInet, kStream | kNonBlock, 0), client = t.s(41, kAfInet, kStream | kNonBlock, 0);
  CHECK(t.s(49, listener, t.in4(127, 0, 0, 1, 9400), 16) == 0 && t.s(50, listener, 8) == 0);
  const int64_t ep = t.s(291, 0);
  const uint64_t event = t.alloc(16), events = t.alloc(12 * 8);
  auto add = [&](int64_t fd, uint32_t mask) { t.put32(event, mask); t.put64(event + 4, uint64_t(fd) | 0xAB00000000ull); return t.s(233, ep, 1, fd, event); };
  CHECK(ep >= 3 && add(listener, 1) == 0 && t.s(232, ep, events, 8, 0) == 0);
  CHECK(t.s(42, client, t.in4(127, 0, 0, 1, 9400), 16) == -115 && add(client, 4) == 0);
  // epoll_wait waits for the connection to arrive at the listener and for the client's connect to be over
  int seen_listener = 0, seen_client = 0;
  for (int round = 0; round < 50 && !(seen_listener && seen_client); ++round) {
    const int64_t n = t.s(232, ep, events, 8, 1000);
    for (int64_t i = 0; i < n; ++i) {
      const uint64_t data = t.u64(events + uint64_t(i) * 12 + 4);
      if (data == (uint64_t(listener) | 0xAB00000000ull) && (t.u32(events + uint64_t(i) * 12) & 1)) seen_listener = 1;
      if (data == (uint64_t(client) | 0xAB00000000ull) && (t.u32(events + uint64_t(i) * 12) & 4)) seen_client = 1;
    }
  }
  CHECK(seen_listener && seen_client);
  const int64_t server = t.s(288, listener, 0, 0, kNonBlock);
  CHECK(server >= 3 && add(server, 1) == 0);
  // more than the stack's buffers hold at once, sent by a socket that does not wait and read as it comes
  std::string sent_text, got_text;
  const uint64_t out = t.alloc(8192), in = t.alloc(8192);
  uint64_t total = 0;
  for (int round = 0; round < 4000 && got_text.size() < 200000; ++round) {
    if (total < 200000) {
      std::string piece(8192, 0);
      for (size_t i = 0; i < piece.size(); ++i) piece[i] = char('a' + (total + i) % 23);
      t.memory.write(out, ConstByteView(reinterpret_cast<const uint8_t*>(piece.data()), piece.size()));
      const int64_t n = t.s(1, client, out, 8192);
      if (n > 0) { sent_text.append(piece, 0, size_t(n)); total += uint64_t(n); }
      else CHECK(n == -11);
    }
    const int64_t n = t.s(0, server, in, 8192);
    if (n > 0) got_text += t.text(in, uint64_t(n));
    else { CHECK(n == -11); t.lan.step(); }
  }
  CHECK(total >= 200000 && got_text == sent_text.substr(0, got_text.size()) && got_text.size() >= 200000);
  // the other way, by a write that waits until all of it is sent while "another thread" reads
  CHECK(t.s(72, server, 4, 0) == 0);
  std::string back;
  t.host.meanwhile = [&] {
    const int64_t n = t.s(0, client, in, 8192);
    if (n > 0) back += t.text(in, uint64_t(n));
  };
  const uint64_t large = t.alloc(100000);
  for (uint64_t i = 0; i < 100000; ++i) t.memory.bytes[large - kBase + i] = uint8_t(i * 7);
  CHECK(t.s(1, server, large, 100000) == 100000);
  t.host.meanwhile = nullptr;
  for (int round = 0; round < 2000 && back.size() < 100000; ++round) {
    const int64_t n = t.s(0, client, in, 8192);
    if (n > 0) back += t.text(in, uint64_t(n));
    else t.lan.step();
  }
  CHECK(back.size() == 100000 && uint8_t(back[99999]) == uint8_t(99999 * 7) && uint8_t(back[12345]) == uint8_t(12345 * 7));
  // the names: both ends are the loopback
  const uint64_t name = t.alloc(32), size = t.word(16);
  CHECK(t.s(52, server, name, size) == 0 && t.text(name + 4, 4) == std::string("\x7f\0\0\x01", 4) && t.s(51, client, name, size) == 0 && t.text(name + 4, 4) == std::string("\x7f\0\0\x01", 4));
  // closing one end is the end of the stream at the other, which epoll reports
  CHECK(t.s(3, client) == 0);
  int closed = 0;
  for (int round = 0; round < 50 && !closed; ++round) {
    const int64_t n = t.s(232, ep, events, 8, 1000);
    for (int64_t i = 0; i < n; ++i)
      if (t.u64(events + uint64_t(i) * 12 + 4) == (uint64_t(server) | 0xAB00000000ull)) closed = 1;
  }
  CHECK(closed && t.s(0, server, in, 64) == 0);
}

// AF_INET6: its own addresses, IPv4 peers through one socket (Linux's default), and IPV6_V6ONLY.
static void test_ipv6() {
  Setup t;
  t.configure_ipv6();
  const network::Ipv6Configuration b6 = t.lan.b.configuration6();
  SharedPointer<network::ITcpSocket> listener = t.lan.b.tcp_open(), peer;
  CHECK(listener->listen_families(9500, network::IpFamilies::Both));
  t.host.meanwhile = [&] { if (!peer) peer = listener->accept(); };
  const int64_t fd = t.s(41, kAfInet6, kStream, 0);
  uint32_t v = 9;
  CHECK(fd >= 3 && t.option(fd, 41, 26, &v) == 0 && v == 0 && t.option(fd, 1, 39, &v) == 0 && v == kAfInet6);      // IPV6_V6ONLY is off
  CHECK(t.s(42, fd, t.in4(10, 0, 0, 2, 9500), 16) == -97);      // a sockaddr_in is not this socket's
  CHECK(t.s(42, fd, t.in6(b6.global, 9500), 28) == 0 && peer && peer->is_connected());
  const uint64_t name = t.alloc(32), size = t.word(28);
  CHECK(t.s(52, fd, name, size) == 0 && t.u32(size) == 28 && t.text(name, 4) == std::string("\x0a\0\x25\x1c", 4));
  CHECK(memcmp(&t.memory.bytes[name - kBase + 8], b6.global.bytes, 16) == 0);
  CHECK(t.s(1, fd, t.bytes("over IPv6"), 9) == 9 && t.lan.run_until([&] { return peer->available() >= 9; }, 2000) && received_by(peer) == "over IPv6");
  // an IPv4 host through an IPv6 socket: ::ffff:10.0.0.2, and its address reads back mapped
  peer.reset();
  const int64_t dual = t.s(41, kAfInet6, kStream, 0);
  CHECK(t.s(42, dual, t.in6(network::Ipv6Address::make(0, 0, 0, 0, 0, 0xFFFF, 0x0A00, 0x0002), 9500), 28) == 0 && peer && !peer->remote_ip().v6);
  t.put32(size, 28);
  CHECK(t.s(52, dual, name, size) == 0 && t.u32(size) == 28 && t.text(name + 18, 6) == std::string("\xff\xff\x0a\0\0\x02", 6));
  // with IPV6_V6ONLY set it is refused; the option cannot change once the socket is in use
  const int64_t only = t.s(41, kAfInet6, kStream, 0);
  CHECK(t.s(54, only, 41, 26, t.word(1), 4) == 0 && t.option(only, 41, 26, &v) == 0 && v == 1);
  CHECK(t.s(42, only, t.in6(network::Ipv6Address::make(0, 0, 0, 0, 0, 0xFFFF, 0x0A00, 0x0002), 9500), 28) == -97);
  CHECK(t.s(54, dual, 41, 26, t.word(1), 4) == -22 && t.s(54, dual, 41, 26, t.word(0), 4) == 0);
  // an AF_INET socket has no IPv6 options; a datagram to an IPv6 address
  const int64_t four = t.s(41, kAfInet, kDgram, 0);
  CHECK(t.s(54, four, 41, 26, t.word(1), 4) == -92 && t.option(four, 41, 26, &v) == -92);
  SharedPointer<network::IUdpSocket> udp_peer = t.lan.b.udp_open_families(9600, network::IpFamilies::Both);
  const int64_t udp = t.s(41, kAfInet6, kDgram, 0);
  CHECK(udp_peer && t.s(44, udp, t.bytes("six"), 3, 0, t.in6(b6.global, 9600), 28) == 3 && t.lan.run_until([&] { return udp_peer->pending() > 0; }, 2000));
  t.host.meanwhile = nullptr;
}

// Options, and everything a program can get wrong.
static void test_options_and_errors() {
  Setup t;
  const int64_t tcp = t.s(41, kAfInet, kStream, 0), udp = t.s(41, kAfInet, kDgram, 0);
  uint32_t v = 0;
  // kept and read back
  CHECK(t.s(54, tcp, 1, 9, t.word(1), 4) == 0 && t.option(tcp, 1, 9, &v) == 0 && v == 1);          // SO_KEEPALIVE
  CHECK(t.s(54, tcp, 6, 1, t.word(1), 4) == 0 && t.option(tcp, 6, 1, &v) == 0 && v == 1);          // TCP_NODELAY
  CHECK(t.s(54, udp, 1, 6, t.word(1), 4) == 0 && t.option(udp, 1, 6, &v) == 0 && v == 1);          // SO_BROADCAST
  CHECK(t.option(tcp, 1, 7, &v) == 0 && v == 212992 && t.s(54, tcp, 1, 7, t.word(65536), 4) == 0 && t.option(tcp, 1, 7, &v) == 0 && v == 131072);      // SO_SNDBUF: doubled, as on Linux
  const uint64_t linger = t.alloc(8), lsize = t.word(8);
  t.put32(linger, 1); t.put32(linger + 4, 5);
  CHECK(t.s(54, tcp, 1, 13, linger, 8) == 0);
  t.put32(linger, 0); t.put32(linger + 4, 0);
  CHECK(t.s(55, tcp, 1, 13, linger, lsize) == 0 && t.u32(linger) == 1 && t.u32(linger + 4) == 5 && t.u32(lsize) == 8);
  CHECK(t.s(54, tcp, 6, 4, t.word(60), 4) == 0 && t.s(54, tcp, 0, 1, t.word(0x10), 4) == 0 && t.option(tcp, 0, 2, &v) == 0 && v == 64);      // TCP_KEEPIDLE, IP_TOS; IP_TTL
  // an answer is cut to the buffer; unknown options and levels
  const uint64_t small = t.alloc(8), ssize = t.word(1);
  CHECK(t.s(55, tcp, 1, 3, small, ssize) == 0 && t.u32(ssize) == 1 && uint8_t(t.text(small, 1)[0]) == 1);
  CHECK(t.option(tcp, 1, 999, &v) == -92 && t.s(54, tcp, 1, 999, t.word(1), 4) == -92 && t.s(54, tcp, 77, 1, t.word(1), 4) == -92 && t.option(tcp, 6, 11, &v) == -92);
  CHECK(t.s(54, udp, 6, 1, t.word(1), 4) == -92 && t.s(54, tcp, 1, 9, t.word(1), 3) == -22 && t.s(54, tcp, 1, 9, kBad, 4) == -14 && t.s(54, tcp, 1, 20, t.word(1), 4) == -22);
  CHECK(t.s(55, tcp, 1, 3, small, kBad) == -14 && t.s(55, tcp, 1, 3, kBad, t.word(4)) == -14 && t.s(54, tcp, 1, 9, t.word(1), uint64_t(-1)) == -22);
  // what cannot be made
  CHECK(t.s(41, kAfInet, 3, 0) == -94 && t.s(41, kAfInet, kStream, 17) == -93 && t.s(41, kAfInet, kDgram, 6) == -93 && t.s(41, kAfInet, kDgram, 1) == -93);
  CHECK(t.s(41, kAfInet, kStream | 0x4000, 0) == -22 && t.s(41, 16 /* AF_NETLINK */, kStream, 0) == -97 && t.s(41, kAfInet, 0, 0) == -22);
  // addresses: every length short of a whole one, a wrong family, a flipped bit anywhere: an error or a result, never anything else
  const uint64_t good = t.in4(10, 0, 0, 2, 9700);
  for (uint64_t length = 0; length < 16; ++length) {
    const int64_t r = t.s(49, udp, good, length);
    CHECK(r == -22 || r == -14);
    CHECK(t.s(44, udp, t.bytes("x"), 1, 0, good, length) == (length == 0 ? -89 : -22));
  }
  CHECK(t.s(49, udp, kBad, 16) == -14 && t.s(42, tcp, kBad, 16) == -14 && t.s(44, udp, t.bytes("x"), 1, 0, kBad, 16) == -14);
  const uint64_t six = t.in6(network::Ipv6Address::make(0x2001, 0xdb8, 0, 0, 0, 0, 0, 1), 9700);
  CHECK(t.s(49, udp, six, 28) == -97 && t.s(42, tcp, six, 28) == -97);
  for (uint32_t bit = 0; bit < 16 * 8; ++bit) {
    t.reuse();
    const uint64_t address = t.in4(127, 0, 0, 1, 9700);
    t.memory.bytes[address - kBase + bit / 8] ^= uint8_t(1u << (bit % 8));
    const int64_t fd = t.s(41, kAfInet, kDgram | kNonBlock, 0);
    const int64_t r = t.s(44, fd, t.bytes("x"), 1, 0, address, 16);
    CHECK(r == 1 || r == -97 || r == -101 || r == -11 || r == -22);
    CHECK(t.s(3, fd) == 0);
  }
  // not a socket; a closed descriptor
  CHECK(t.s(49, 1, good, 16) == -88 && t.s(42, 999, good, 16) == -9 && t.s(50, 1, 1) == -88 && t.s(48, 999, 0) == -9 && t.s(48, tcp, 0) == -107);
  CHECK(t.s(8, tcp, 0, 0) == -29 && t.s(17, tcp, t.alloc(8), 8, 0) == -29);      // lseek, pread: ESPIPE
  // as many sockets as there are, and the room comes back when they are closed
  int made = 0;
  int64_t last = -1;
  for (;;) {
    const int64_t fd = t.s(41, kAfInet, kDgram, 0);
    if (fd < 0) { CHECK(fd == -23 || fd == -24); break; }
    last = fd;
    ++made;
  }
  CHECK(made > 200 && t.s(3, last) == 0 && t.s(41, kAfInet, kDgram, 0) == last);
  // no network at all: the family is not there
  set_system_sockets(nullptr);
  Process bare{8, ProcessBinding{SyscallAbi::Linux, UserArch::X86_64, syscall_interface(SyscallAbi::Linux, UserArch::X86_64)}, t.memory, t.host};
  SyscallRequest r;
  r.number = 41;
  r.args[0] = kAfInet; r.args[1] = kStream;
  CHECK(int64_t(bare.syscall(r)) == -97);
  set_system_sockets(&t.sockets);
}

// Finding hosts by name: the files a C library reads are written into the program's tree from the network configuration, and the DNS server named there is
// asked the way a resolver asks it.
static void test_name_resolution() {
  Setup t;
  filesystem::RamFileSystem fs(t.lan.alloc);
  CHECK(fs.create_directory("/Linux") == filesystem::Status::Ok);
  t.process.attach_file_system(t.lan.alloc, fs);
  auto set_dns = [&](uint8_t last) {
    network::IpConfiguration c = t.lan.a.configuration();
    c.dns = network::Ipv4Address::make(10, 0, 0, last);
    t.lan.a.configure(c);
  };
  auto contents = [&](const char* path) {
    const int64_t fd = t.s(2, t.bytes(path), 0);
    if (fd < 0) return std::string("<error ") + std::to_string(fd) + ">";
    const uint64_t buf = t.alloc(8192);
    const int64_t n = t.s(0, fd, buf, 8192);
    t.s(3, fd);
    return n < 0 ? std::string("<unreadable>") : t.text(buf, uint64_t(n));
  };
  const std::string marker = "# Written by TheoryCraftOS from the network configuration. A file without this line is left alone.\n";
  set_dns(2);
  // resolv.conf is there the moment a program names it (also for stat and access), with the interface's DNS server
  const uint64_t st = t.alloc(160);
  CHECK(t.s(4, t.bytes("/etc/resolv.conf"), st) == 0 && (t.u32(st + 24) & 0170000) == 0100000 && t.u64(st + 48) == marker.size() + 20);
  CHECK(contents("/etc/resolv.conf") == marker + "nameserver 10.0.0.2\n" && t.s(21, t.bytes("/etc/resolv.conf"), 4) == 0);
  // ... and follows the network configuration
  set_dns(9);
  CHECK(contents("/etc/resolv.conf") == marker + "nameserver 10.0.0.9\n");
  set_dns(2);
  CHECK(contents("/etc/../etc/./resolv.conf") == marker + "nameserver 10.0.0.2\n");
  // the companions: written once, with what a resolver needs in them
  const std::string hosts = contents("/etc/hosts"), order = contents("/etc/nsswitch.conf"), services = contents("/etc/services"), protocols = contents("/etc/protocols");
  CHECK(hosts.find("127.0.0.1 localhost\n") == 0 && hosts.find("::1 localhost") != std::string::npos && hosts.find("127.0.1.1 tcos\n") != std::string::npos);
  CHECK(order.find("hosts: files dns\n") != std::string::npos && services.find("\nhttp 80/tcp") != std::string::npos && services.find("\ndomain 53/udp\n") != std::string::npos);
  CHECK(protocols.find("tcp 6 TCP\n") != std::string::npos && protocols.find("udp 17 UDP\n") != std::string::npos);
  CHECK(contents("/etc/passwd") == "<error -2>" && contents("/tmp/resolv.conf") == "<error -2>" && contents("/etc/resolv.conf.bak") == "<error -2>");      // nothing else is made up
  // a file somebody else wrote is theirs: it is not written over, whatever the network says
  auto write = [&](const char* path, const std::string& text) {
    const int64_t fd = t.s(2, t.bytes(path), 0x241 /* O_WRONLY | O_CREAT | O_TRUNC */, 0644);
    const bool ok = fd >= 0 && t.s(1, fd, t.bytes(text), text.size()) == int64_t(text.size());
    t.s(3, fd);
    return ok;
  };
  CHECK(write("/etc/hosts", "10.9.8.7 mine\n") && contents("/etc/hosts") == "10.9.8.7 mine\n");
  CHECK(write("/etc/resolv.conf", "nameserver 9.9.9.9\n"));
  set_dns(5);
  CHECK(contents("/etc/resolv.conf") == "nameserver 9.9.9.9\n");
  // (removed, it is written again; so is one that still has the first line and something stale after it)
  CHECK(t.s(87, t.bytes("/etc/resolv.conf")) == 0 && contents("/etc/resolv.conf") == marker + "nameserver 10.0.0.5\n");
  CHECK(write("/etc/resolv.conf", marker + "nameserver 1.2.3.4\noptions rotate\n") && contents("/etc/resolv.conf") == marker + "nameserver 10.0.0.5\n");
  CHECK(write("/etc/resolv.conf", marker.substr(0, 40)) && contents("/etc/resolv.conf") == marker.substr(0, 40));      // (a cut-off first line is not the marker)
  CHECK(t.s(87, t.bytes("/etc/resolv.conf")) == 0);
  set_dns(2);

  // a resolver's exchange with the server the file names: a datagram socket connected to port 53, a question, the answer
  const std::string file = contents("/etc/resolv.conf");
  CHECK(file == marker + "nameserver 10.0.0.2\n");
  SharedPointer<network::IUdpSocket> server = t.lan.b.udp_open(53);
  CHECK(server);
  bool answered = false;
  t.host.meanwhile = [&] {
    uint8_t question[512];
    network::IpAddress from;
    uint16_t from_port = 0;
    uint32_t length = 0;
    if (answered || !server->receive_from_ip(ByteView(question, sizeof(question)), &from, &from_port, &length)) return;
    answered = true;
    question[2] |= 0x80;      // (the same message, marked as an answer)
    server->send_to_ip(from, from_port, ConstByteView(question, length));
  };
  const int64_t fd = t.s(41, kAfInet, kDgram | kNonBlock | kCloexec, 0);
  CHECK(fd >= 3 && t.s(54, fd, 0, 11 /* IP_RECVERR */, t.word(1), 4) == 0 && t.s(42, fd, t.in4(10, 0, 0, 2, 53), 16) == 0);
  const std::string query("\x12\x34\x01\x00\x00\x01\x00\x00\x00\x00\x00\x00\x04host\x04test\x00\x00\x01\x00\x01", 27);
  const uint64_t pfd = t.alloc(8), buf = t.alloc(2048), waiting = t.word(0);
  t.put32(pfd, uint32_t(fd)); t.put32(pfd + 4, kPollOut);
  CHECK(t.s(7, pfd, 1, 0) == 1 && t.s(44, fd, t.bytes(query), query.size(), 0x4000 /* MSG_NOSIGNAL */, 0, 0) == int64_t(query.size()));
  t.put32(pfd + 4, kPollIn);
  CHECK(t.s(7, pfd, 1, 5000) == 1 && (t.u32(pfd + 4) >> 16) == kPollIn && t.s(16, fd, 0x541B, waiting) == 0 && t.u32(waiting) == 1472);      // FIONREAD: room for any datagram
  const uint64_t name = t.alloc(32), size = t.word(28);
  CHECK(t.s(45, fd, buf, 2048, 0, name, size) == int64_t(query.size()) && uint8_t(t.text(buf + 2, 1)[0]) == 0x81 && t.text(buf, 2) == "\x12\x34");
  CHECK(t.text(name + 2, 6) == std::string("\x00\x35\x0a\0\0\x02", 6) && t.s(16, fd, 0x541B, waiting) == 0 && t.u32(waiting) == 0);      // from port 53 of the server
  t.host.meanwhile = nullptr;
  // the same socket for another host: connect() with an address of no family takes the peer away
  const uint64_t unspecified = t.alloc(16);
  CHECK(t.s(52, fd, name, size) == 0 && t.s(42, fd, unspecified, 16) == 0 && t.s(52, fd, name, size) == -107 && t.s(1, fd, t.bytes("x"), 1) == -89);
  CHECK(t.s(42, fd, t.in4(10, 0, 0, 2, 53), 16) == 0 && t.s(52, fd, name, size) == 0 && t.s(42, fd, kBad, 16) == -14);
  const int64_t tcp = t.s(41, kAfInet, kStream, 0);
  CHECK(t.s(42, tcp, unspecified, 16) == -97);      // (not for a stream socket)
  // with no network at all the file is there and names no server
  set_system_sockets(nullptr);
  CHECK(contents("/etc/resolv.conf") == marker);
  set_system_sockets(&t.sockets);
  CHECK(contents("/etc/resolv.conf") == marker + "nameserver 10.0.0.2\n");
  t.process.release_handles();
}

// MSG_PEEK: what is looked at is received again, in the same order, by whatever reads next.
static void test_peek() {
  constexpr uint64_t kPeek = 2, kDontWait = 0x40, kWaitAll = 0x100;
  Setup t;
  const int64_t listener = t.s(41, kAfInet, kStream | kNonBlock, 0), client = t.s(41, kAfInet, kStream | kNonBlock, 0);
  CHECK(t.s(49, listener, t.in4(127, 0, 0, 1, 9800), 16) == 0 && t.s(50, listener, 4) == 0 && t.s(42, client, t.in4(127, 0, 0, 1, 9800), 16) == -115);
  CHECK(t.lan.run_until([&] { return t.polled(listener, kPollIn) == kPollIn; }, 3000));
  const int64_t server = t.s(288, listener, 0, 0, kNonBlock);
  const uint64_t buf = t.alloc(256), waiting = t.word(0);
  auto arrives = [&](int64_t fd, uint32_t bytes) { return t.lan.run_until([&] { return t.s(16, fd, 0x541B, waiting) == 0 && t.u32(waiting) >= bytes; }, 3000); };
  CHECK(server >= 3 && t.s(45, server, buf, 64, kPeek, 0, 0) == -11);      // nothing there yet
  CHECK(t.s(1, client, t.bytes("hello world"), 11) == 11 && arrives(server, 11));
  // a stream: looked at as often as wanted, a little or all of it, then read from the start
  CHECK(t.s(45, server, buf, 5, kPeek, 0, 0) == 5 && t.text(buf, 5) == "hello" && t.s(45, server, buf, 64, kPeek, 0, 0) == 11 && t.text(buf, 11) == "hello world");
  CHECK(t.s(16, server, 0x541B, waiting) == 0 && t.u32(waiting) == 11 && t.polled(server, kPollIn) == kPollIn);      // still all there
  CHECK(t.s(0, server, buf, 6) == 6 && t.text(buf, 6) == "hello " && t.s(45, server, buf, 64, kPeek | kWaitAll, 0, 0) == 5 && t.text(buf, 5) == "world");
  // more arrives while some is held: a look sees both, in order; so does a read through several buffers
  CHECK(t.s(1, client, t.bytes(", again"), 7) == 7 && arrives(server, 12));
  CHECK(t.s(45, server, buf, 64, kPeek, 0, 0) == 12 && t.text(buf, 12) == "world, again" && t.u32(waiting) == 12);
  const uint64_t r1 = t.alloc(8), r2 = t.alloc(16), vectors = t.alloc(32), message = t.alloc(56);
  t.put64(vectors, r1); t.put64(vectors + 8, 5); t.put64(vectors + 16, r2); t.put64(vectors + 24, 16);
  t.put64(message, 0); t.put64(message + 8, 0); t.put64(message + 16, vectors); t.put64(message + 24, 2); t.put64(message + 32, 0); t.put64(message + 40, 0); t.put64(message + 48, 0);
  CHECK(t.s(47, server, message, kPeek) == 12 && t.text(r1, 5) == "world" && t.text(r2, 7) == ", again");
  CHECK(t.s(19, server, vectors, 2) == 12 && t.text(r1, 5) == "world" && t.text(r2, 7) == ", again");
  CHECK(t.s(45, server, buf, 64, kPeek, 0, 0) == -11 && t.s(0, server, buf, 64) == -11 && t.polled(server, kPollIn) == 0);      // all read: nothing is held
  // a lot: a look takes what one call may hold; reading afterwards gets everything, in order
  std::string sent(100000, 0), got;
  for (size_t i = 0; i < sent.size(); ++i) sent[i] = char(i * 13 + i / 251);
  const uint64_t large = t.alloc(100000), in = t.alloc(70000);
  t.memory.write(large, ConstByteView(reinterpret_cast<const uint8_t*>(sent.data()), sent.size()));
  uint64_t written = 0;
  int64_t looked = 0;
  for (int round = 0; round < 6000 && got.size() < sent.size(); ++round) {
    if (written < sent.size()) {
      const int64_t n = t.s(1, client, large + written, sent.size() - written);
      if (n > 0) written += uint64_t(n);
    }
    if (round % 3 == 0 || looked < 40000) {
      const int64_t seen = t.s(45, server, in, 70000, kPeek, 0, 0);
      if (seen > 0) {
        CHECK(seen <= 65536 && t.text(in, uint64_t(seen)) == sent.substr(got.size(), size_t(seen)));
        if (seen > looked) looked = seen;
      }
    }
    // (nothing is read until a look has seen a good deal more than the stack's own buffer holds: looking again takes more out of it)
    if (looked < 40000 && round < 3000) { t.lan.step(); continue; }
    const int64_t n = t.s(0, server, in, 3000 + round % 5000);
    if (n > 0) got += t.text(in, uint64_t(n));
    else t.lan.step();
  }
  CHECK(got == sent && looked >= 40000);
  // a look that waits for something to come; the end of the stream after what was looked at
  CHECK(t.s(72, server, 4, 0) == 0);
  bool wrote = false;
  t.host.meanwhile = [&] { if (!wrote) { wrote = true; CHECK(t.s(1, client, t.bytes("last"), 4) == 4); } };
  CHECK(t.s(45, server, buf, 64, kPeek, 0, 0) == 4 && t.text(buf, 4) == "last" && t.s(45, server, buf, 64, kPeek | kDontWait, 0, 0) == 4);
  t.host.meanwhile = nullptr;
  CHECK(t.s(3, client) == 0 && t.lan.run_until([&] { return t.sockets.peer_closed(uint32_t(t.process.files().token(uint64_t(server)))); }, 3000));
  CHECK(t.s(45, server, buf, 2, kPeek, 0, 0) == 2 && t.s(0, server, buf, 64) == 4 && t.text(buf, 4) == "last");
  CHECK(t.s(45, server, buf, 64, kPeek, 0, 0) == 0 && t.s(0, server, buf, 64) == 0);      // the end can be looked at too

  // datagrams: a look shows the next one, cut to the buffer, and leaves it whole for the receive
  SharedPointer<network::IUdpSocket> peer = t.lan.b.udp_open(9810);
  const int64_t udp = t.s(41, kAfInet, kDgram | kNonBlock, 0);
  CHECK(t.s(49, udp, t.in4(0, 0, 0, 0, 9811), 16) == 0 && t.s(45, udp, buf, 64, kPeek, 0, 0) == -11);
  const network::IpAddress here(network::Ipv4Address::make(10, 0, 0, 1));
  CHECK(peer->send_to_ip(here, 9811, ConstByteView(reinterpret_cast<const uint8_t*>("abcdef"), 6)) && peer->send_to_ip(here, 9811, ConstByteView(reinterpret_cast<const uint8_t*>("second"), 6)));
  CHECK(t.lan.run_until([&] { return t.sockets.readable(uint32_t(t.process.files().token(uint64_t(udp)))) == 2; }, 3000));
  const uint64_t name = t.alloc(32), size = t.word(28);
  CHECK(t.s(45, udp, buf, 3, kPeek, name, size) == 3 && t.text(buf, 3) == "abc" && t.u32(size) == 16 && t.text(name + 2, 6) == std::string("\x26\x52\x0a\0\0\x02", 6));
  CHECK(t.s(45, udp, buf, 64, kPeek, 0, 0) == 6 && t.text(buf, 6) == "abcdef" && t.polled(udp, kPollIn) == kPollIn && t.s(16, udp, 0x541B, waiting) == 0 && t.u32(waiting) == 1472);
  t.put32(size, 28);
  CHECK(t.s(45, udp, buf, 64, 0, name, size) == 6 && t.text(buf, 6) == "abcdef" && t.text(name + 2, 6) == std::string("\x26\x52\x0a\0\0\x02", 6));      // whole, with its sender
  CHECK(t.s(45, udp, buf, 64, 0, 0, 0) == 6 && t.text(buf, 6) == "second" && t.s(45, udp, buf, 64, kPeek, 0, 0) == -11 && t.polled(udp, kPollIn) == 0);
  // a socket closed with something held gives its memory back (the test's allocator counts)
  CHECK(peer->send_to_ip(here, 9811, ConstByteView(reinterpret_cast<const uint8_t*>("held"), 4)) && t.lan.run_until([&] { return t.polled(udp, kPollIn) == kPollIn; }, 3000));
  CHECK(t.s(45, udp, buf, 64, kPeek, 0, 0) == 4 && t.s(3, udp) == 0 && t.s(3, server) == 0);
}

int main() {
  test_tcp_client();
  test_half_close();
  test_connecting();
  test_tcp_server();
  test_udp();
  test_loopback_and_epoll();
  test_ipv6();
  test_options_and_errors();
  test_name_resolution();
  test_peek();
  if (g_failed) { printf("LinuxInetSocketTest: %d check(s) failed\n", g_failed); return 1; }
  printf("LinuxInetSocketTest: all passed\n");
  return 0;
}
