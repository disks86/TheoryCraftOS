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
#include <map>
#include <string>
#include <vector>
#include "NetworkHarness.hpp"
#include "Ipv6Harness.hpp"
#include "FileSystem/Ram/RamFileSystem.hpp"
#include "Process/LinuxBpf.hpp"
#include "Process/LinuxPacket.hpp"
#include "Process/LinuxPacketRing.hpp"
#include "Process/LinuxIpc.hpp"
#include "Process/LinuxNetlink.hpp"
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
  LinuxIpcManager ipc{sync, lan.alloc};
  Process process{7, ProcessBinding{SyscallAbi::Linux, UserArch::X86_64, syscall_interface(SyscallAbi::Linux, UserArch::X86_64)}, memory, host};
  uint64_t cursor = kBase + 0x1000;
  Setup() {
    lan.configure_static();
    set_system_sockets(&sockets);
    set_system_unix_sockets(&descriptors);
    set_system_linux_ipc(&ipc);
    host.lan = &lan;
    runtime.host = &host;
    sync.host = &host;
    process.attach_runtime(&runtime);
  }
  ~Setup() {
    process.release_handles();
    set_system_linux_ipc(nullptr);
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
  CHECK(t.s(47, fd, message, 0) == 7 && t.text(r1, 3) == "ABC" && t.text(r2, 4) == "DEFG" && t.u32(message + 8) == 16 && t.u64(message + 40) == 0 && t.u32(message + 48) == 0x20 /* MSG_TRUNC: three bytes did not fit */);
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
  CHECK(t.s(41, kAfInet, 4 /* SOCK_RDM */, 0) == -94 && t.s(41, kAfInet, kStream, 17) == -93 && t.s(41, kAfInet, kDgram, 6) == -93);
  CHECK(t.s(41, kAfInet, kStream | 0x4000, 0) == -22 && t.s(41, 4 /* AF_IPX */, kStream, 0) == -97 && t.s(41, kAfInet, 0, 0) == -22);
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

// Control messages: what recvmsg says about a datagram when the socket asked, and what sendmsg takes.
static void test_control_messages() {
  constexpr uint64_t kTrunc = 0x20, kCtrunc = 8;
  Setup t;
  t.configure_ipv6();
  SharedPointer<network::IUdpSocket> peer = t.lan.b.udp_open_families(9900, network::IpFamilies::Both);
  const network::IpAddress here(network::Ipv4Address::make(10, 0, 0, 1));
  const int64_t fd = t.s(41, kAfInet, kDgram, 0);
  CHECK(peer && t.s(49, fd, t.in4(0, 0, 0, 0, 9901), 16) == 0);
  // recvmsg into one buffer, with room for `capacity` bytes of control messages; what came back
  struct Got { int64_t result = 0; uint64_t control_length = 0; uint32_t flags = 0; std::string control, data; };
  const uint64_t data = t.alloc(64), control = t.alloc(512), vector = t.alloc(16), message = t.alloc(56), name = t.alloc(32);
  auto receive = [&](int64_t socket, uint64_t room, uint64_t capacity, uint64_t flags = 0, bool with_control = true) {
    t.put64(vector, data); t.put64(vector + 8, room);
    t.put64(message, name); t.put32(message + 8, 28); t.put64(message + 16, vector); t.put64(message + 24, 1);
    t.put64(message + 32, with_control ? control : 0); t.put64(message + 40, capacity); t.put32(message + 48, 0xEEEE);
    memset(&t.memory.bytes[control - kBase], 0xEE, 512);
    Got g;
    g.result = t.s(47, socket, message, flags);
    g.control_length = t.u64(message + 40);
    g.flags = t.u32(message + 48);
    if (g.result >= 0) {
      g.control = t.text(control, g.control_length <= 512 ? g.control_length : 0);
      g.data = t.text(data, uint64_t(g.result) < room ? uint64_t(g.result) : room);
    }
    return g;
  };
  // the control messages in a buffer: level, type and data of each
  struct Message { uint32_t level = 0, type = 0; std::string data; };
  auto parse = [](const std::string& bytes) {
    std::vector<Message> out;
    for (size_t at = 0; at + 16 <= bytes.size();) {
      uint64_t length = 0;
      for (int i = 7; i >= 0; --i) length = (length << 8) | uint8_t(bytes[at + size_t(i)]);
      if (length < 16 || length > bytes.size() - at) break;
      Message m;
      for (int i = 3; i >= 0; --i) { m.level = (m.level << 8) | uint8_t(bytes[at + 8 + size_t(i)]); m.type = (m.type << 8) | uint8_t(bytes[at + 12 + size_t(i)]); }
      m.data = bytes.substr(at + 16, size_t(length) - 16);
      out.push_back(m);
      at += (size_t(length) + 7) & ~size_t{7};
    }
    return out;
  };
  auto send_to_here = [&](const char* text, uint16_t port = 9901) {
    return peer->send_to_ip(here, port, ConstByteView(reinterpret_cast<const uint8_t*>(text), strlen(text))) && t.lan.run_until([&] { return t.polled(fd, kPollIn) == kPollIn; }, 3000);
  };
  // nothing was asked for: nothing comes, and the lengths say so
  CHECK(send_to_here("plain"));
  Got g = receive(fd, 64, 256);
  CHECK(g.result == 5 && g.data == "plain" && g.control_length == 0 && g.flags == 0 && t.u32(message + 8) == 16);
  // asked for: when it came, where it arrived, its TTL and TOS, in that order
  uint32_t v = 0;
  CHECK(t.s(54, fd, 1, 29, t.word(1), 4) == 0 && t.s(54, fd, 0, 8, t.word(1), 4) == 0 && t.s(54, fd, 0, 12, t.word(1), 4) == 0 && t.s(54, fd, 0, 13, t.word(1), 4) == 0);
  CHECK(t.option(fd, 1, 29, &v) == 0 && v == 1 && t.option(fd, 0, 8, &v) == 0 && v == 1 && t.option(fd, 0, 12, &v) == 0 && v == 1 && t.option(fd, 0, 13, &v) == 0 && v == 1);
  CHECK(send_to_here("with everything"));
  g = receive(fd, 64, 256);
  std::vector<Message> m = parse(g.control);
  CHECK(g.result == 15 && g.flags == 0 && g.control_length == 32 + 32 + 24 + 24 && m.size() == 4);
  if (m.size() == 4) {
    CHECK(m[0].level == 1 && m[0].type == 29 && m[0].data.size() == 16);                                                                   // SCM_TIMESTAMP: a struct timeval
    CHECK(m[1].level == 0 && m[1].type == 8 && m[1].data == std::string("\x02\0\0\0\x0a\0\0\x01\x0a\0\0\x01", 12));                        // IP_PKTINFO: interface 2, to 10.0.0.1
    CHECK(m[2].level == 0 && m[2].type == 2 && m[2].data == std::string("\x40\0\0\0", 4) && m[3].level == 0 && m[3].type == 1 && m[3].data == std::string(1, '\0'));      // IP_TTL 64, IP_TOS 0
  }
  // a buffer too small for all of it gets the ones that fit, and MSG_CTRUNC; none at all is the same
  CHECK(send_to_here("small buffer") && send_to_here("no buffer") && send_to_here("no pointer"));
  g = receive(fd, 64, 70);      // (the third one's header does not fit)
  m = parse(g.control);
  CHECK(g.result == 12 && g.flags == kCtrunc && g.control_length == 64 && m.size() == 2 && m[1].type == 8);
  g = receive(fd, 64, 0);
  CHECK(g.result == 9 && g.flags == kCtrunc && g.control_length == 0);
  g = receive(fd, 64, 256, 0, false);
  CHECK(g.result == 10 && g.flags == kCtrunc && g.control_length == 0);
  CHECK(send_to_here("half a message"));
  g = receive(fd, 64, 32 + 20);      // (the second one's header does, and four bytes of it)
  m = parse(g.control);
  CHECK(g.result == 14 && g.flags == kCtrunc && g.control_length == 52 && m.size() == 2 && m[1].type == 8 && m[1].data == std::string("\x02\0\0\0", 4));
  CHECK(send_to_here("under a header"));
  g = receive(fd, 64, 12);
  CHECK(g.result == 14 && g.flags == kCtrunc && g.control_length == 0);
  // a datagram longer than the buffer: MSG_TRUNC says so, and asked for (in the flags) the call answers with its real size
  CHECK(send_to_here("0123456789") && send_to_here("abcdefghij") && send_to_here("ABCDEFGHIJ") && send_to_here("klmnopqrst"));
  g = receive(fd, 4, 256);
  CHECK(g.result == 4 && g.data == "0123" && (g.flags & kTrunc) != 0 && (g.flags & kCtrunc) == 0);
  g = receive(fd, 4, 256, kTrunc);
  CHECK(g.result == 10 && g.data == "abcd" && (g.flags & kTrunc) != 0);
  CHECK(t.s(45, fd, data, 3, kTrunc, 0, 0) == 10 && t.text(data, 3) == "ABC" && t.s(45, fd, data, 3, kTrunc | 2 /* MSG_PEEK */, 0, 0) == 10 && t.s(45, fd, data, 64, 0, 0, 0) == 10);
  // SO_TIMESTAMPNS in place of SO_TIMESTAMP; the options turned off again
  CHECK(t.s(54, fd, 1, 35, t.word(1), 4) == 0 && t.option(fd, 1, 29, &v) == 0 && v == 0 && t.option(fd, 1, 35, &v) == 0 && v == 1 && send_to_here("ns"));
  g = receive(fd, 64, 256);
  m = parse(g.control);
  CHECK(m.size() == 4 && m[0].level == 1 && m[0].type == 35 && m[0].data.size() == 16);
  CHECK(t.s(54, fd, 1, 35, t.word(0), 4) == 0 && t.s(54, fd, 0, 12, t.word(0), 4) == 0 && t.s(54, fd, 0, 13, t.word(0), 4) == 0 && send_to_here("less"));
  g = receive(fd, 64, 256);
  m = parse(g.control);
  CHECK(g.control_length == 32 && m.size() == 1 && m[0].type == 8);
  // over the loopback: interface 1, the loopback address
  const int64_t local = t.s(41, kAfInet, kDgram, 0);
  CHECK(t.s(44, local, t.bytes("loop"), 4, 0, t.in4(127, 0, 0, 1, 9901), 16) == 4 && t.lan.run_until([&] { return t.polled(fd, kPollIn) == kPollIn; }, 3000));
  g = receive(fd, 64, 256);
  m = parse(g.control);
  CHECK(g.result == 4 && m.size() == 1 && m[0].data == std::string("\x01\0\0\0\x7f\0\0\x01\x7f\0\0\x01", 12));
  // the error queue is always empty; a stream socket's recvmsg has no control messages
  CHECK(t.s(47, fd, message, 0x2000 /* MSG_ERRQUEUE */) == -11 && t.s(45, fd, data, 8, 0x2000, 0, 0) == -11);

  // IPv6: IPV6_PKTINFO, the hop limit and the traffic class; an IPv4 datagram through the same socket is shown mapped
  const network::Ipv6Configuration a6 = t.lan.a.configuration6(), b6 = t.lan.b.configuration6();
  const int64_t six = t.s(41, kAfInet6, kDgram, 0);
  CHECK(t.s(49, six, t.in6(network::kIpv6Any, 9902), 28) == 0 && t.s(54, six, 41, 49, t.word(1), 4) == 0 && t.s(54, six, 41, 51, t.word(1), 4) == 0 && t.s(54, six, 41, 66, t.word(1), 4) == 0);
  CHECK(t.option(six, 41, 49, &v) == 0 && v == 1 && t.option(six, 41, 51, &v) == 0 && v == 1 && t.option(six, 41, 66, &v) == 0 && v == 1);
  CHECK(peer->send_to_ip(network::IpAddress(a6.global), 9902, ConstByteView(reinterpret_cast<const uint8_t*>("six"), 3)) && t.lan.run_until([&] { return t.polled(six, kPollIn) == kPollIn; }, 3000));
  g = receive(six, 64, 256);
  m = parse(g.control);
  CHECK(g.result == 3 && g.control_length == 40 + 24 + 24 && m.size() == 3 && t.u32(message + 8) == 28);
  if (m.size() == 3) {
    CHECK(m[0].level == 41 && m[0].type == 50 && m[0].data.size() == 20 && memcmp(m[0].data.data(), a6.global.bytes, 16) == 0 && m[0].data.substr(16) == std::string("\x02\0\0\0", 4));
    CHECK(m[1].level == 41 && m[1].type == 52 && m[1].data == std::string("\x40\0\0\0", 4) && m[2].level == 41 && m[2].type == 67 && m[2].data == std::string(4, '\0'));
  }
  CHECK(t.s(54, six, 0, 8, t.word(1), 4) == 0 && peer->send_to_ip(here, 9902, ConstByteView(reinterpret_cast<const uint8_t*>("four"), 4)) &&
        t.lan.run_until([&] { return t.polled(six, kPollIn) == kPollIn; }, 3000));
  g = receive(six, 64, 256);
  m = parse(g.control);
  CHECK(g.result == 4 && m.size() == 2 && m[0].level == 41 && m[0].type == 50 && m[0].data.substr(10, 6) == std::string("\xff\xff\x0a\0\0\x01", 6));      // its address mapped
  CHECK(m.size() == 2 && m[1].level == 0 && m[1].type == 8 && m[1].data == std::string("\x02\0\0\0\x0a\0\0\x01\x0a\0\0\x01", 12));                      // then IP_PKTINFO
  CHECK(t.s(54, fd, 41, 49, t.word(1), 4) == -92 && t.option(fd, 41, 49, &v) == -92);      // (no IPv6 options on an AF_INET socket)
  (void)b6;

  // sendmsg: control messages a program may pass are taken (and the datagram goes out); anything else is refused, as is one that is not whole
  auto cmsg = [&](std::string* out, uint32_t level, uint32_t type, const std::string& payload, int64_t length_override = -1) {
    const uint64_t length = length_override >= 0 ? uint64_t(length_override) : 16 + payload.size();
    for (int i = 0; i < 8; ++i) out->push_back(char(length >> (8 * i)));
    for (int i = 0; i < 4; ++i) out->push_back(char(level >> (8 * i)));
    for (int i = 0; i < 4; ++i) out->push_back(char(type >> (8 * i)));
    *out += payload;
    while (out->size() % 8 != 0) out->push_back('\0');
  };
  auto send_with = [&](int64_t socket, const std::string& bytes, uint64_t destination, uint64_t destination_size, uint64_t length = ~uint64_t{0}) {
    const uint64_t at = t.alloc(bytes.size() + 8), text = t.bytes("payload");
    t.memory.write(at, ConstByteView(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()));
    t.put64(vector, text); t.put64(vector + 8, 7);
    t.put64(message, destination); t.put32(message + 8, uint32_t(destination_size)); t.put64(message + 16, vector); t.put64(message + 24, 1);
    t.put64(message + 32, at); t.put64(message + 40, length == ~uint64_t{0} ? bytes.size() : length); t.put32(message + 48, 0);
    return t.s(46, socket, message, 0);
  };
  const uint64_t to_peer = t.in4(10, 0, 0, 2, 9900);
  std::string good;
  cmsg(&good, 0, 8, std::string("\0\0\0\0\x0a\0\0\x01\0\0\0\0", 12));      // IP_PKTINFO: from 10.0.0.1
  cmsg(&good, 0, 2, std::string("\x10\0\0\0", 4));                         // IP_TTL
  cmsg(&good, 0, 1, std::string("\x08", 1));                              // IP_TOS (one byte)
  CHECK(send_with(fd, good, to_peer, 16) == 7 && t.lan.run_until([&] { return peer->pending() > 0; }, 3000));
  uint8_t buffer[64];
  network::IpAddress from;
  uint16_t from_port = 0;
  uint32_t length = 0;
  CHECK(peer->receive_from_ip(ByteView(buffer, sizeof(buffer)), &from, &from_port, &length) && length == 7 && from_port == 9901);
  std::string bad;
  cmsg(&bad, 0, 8, std::string(8, '\0'));                                  // IP_PKTINFO too short
  CHECK(send_with(fd, bad, to_peer, 16) == -22);
  bad.clear(); cmsg(&bad, 0, 99, std::string(4, '\0'));                    // an option nobody knows
  CHECK(send_with(fd, bad, to_peer, 16) == -22);
  bad.clear(); cmsg(&bad, 0, 2, std::string(4, '\0'));                     // a TTL of nothing
  CHECK(send_with(fd, bad, to_peer, 16) == -22);
  bad.clear(); cmsg(&bad, 1, 99, std::string(4, '\0'));                    // a socket-level one nobody knows
  CHECK(send_with(fd, bad, to_peer, 16) == -22);
  bad.clear(); cmsg(&bad, 17, 103, std::string("\x00\x05\x00", 3));        // UDP_SEGMENT of the wrong size
  CHECK(send_with(fd, bad, to_peer, 16) == -22);
  bad.clear(); cmsg(&bad, 0, 2, std::string("\x10\0\0\0", 4), 15);         // a length less than the header
  CHECK(send_with(fd, bad, to_peer, 16) == -22);
  bad.clear(); cmsg(&bad, 0, 2, std::string("\x10\0\0\0", 4), 4096);       // a length past the end
  CHECK(send_with(fd, bad, to_peer, 16) == -22);
  // passed over, as on Linux: descriptors and credentials (they mean something over AF_UNIX only), another family's messages, less than a header
  std::string other;
  cmsg(&other, 1, 1, std::string(4, '\0'));                                // SCM_RIGHTS
  cmsg(&other, 41, 50, std::string(20, '\0'));                             // IPV6_PKTINFO to an AF_INET socket
  cmsg(&other, 1, 36, std::string(4, '\0'));                               // SO_MARK
  CHECK(send_with(fd, other, to_peer, 16) == 7 && send_with(six, good, t.in6(b6.global, 9900), 28) == 7 && send_with(fd, good, to_peer, 16, 8) == 7 && send_with(fd, good, to_peer, 16, 0) == 7);
  std::string good6;
  cmsg(&good6, 41, 50, std::string(20, '\0'));
  cmsg(&good6, 41, 52, std::string("\xff\xff\xff\xff", 4));               // IPV6_HOPLIMIT -1: the usual one
  CHECK(send_with(six, good6, t.in6(b6.global, 9900), 28) == 7);
  bad.clear(); cmsg(&bad, 41, 52, std::string("\0\x01\0\0", 4));           // a hop limit of 256
  CHECK(send_with(six, bad, t.in6(b6.global, 9900), 28) == -22);
  CHECK(send_with(fd, std::string(20488, '\0'), to_peer, 16) == -105 && send_with(fd, std::string(8200, '\0'), to_peer, 16) == -22 && t.s(46, fd, kBad, 0) == -14);
  t.put64(message + 32, kBad); t.put64(message + 40, 32);
  CHECK(t.s(46, fd, message, 0) == -14);
  // every way of cutting a good buffer short, and every bit of it flipped: taken or refused, nothing else
  int sent = 0, refused = 0;
  for (uint64_t cut = 0; cut <= good.size(); ++cut) {
    t.reuse();
    const int64_t r = send_with(fd, good, t.in4(10, 0, 0, 2, 9900), 16, cut);
    CHECK(r == 7 || r == -22);
    (r == 7 ? sent : refused) += 1;
  }
  for (size_t bit = 0; bit < good.size() * 8; ++bit) {
    t.reuse();
    std::string flipped = good;
    flipped[bit / 8] = char(flipped[bit / 8] ^ (1 << (bit % 8)));
    const int64_t r = send_with(fd, flipped, t.in4(10, 0, 0, 2, 9900), 16);
    CHECK(r == 7 || r == -22);
    (r == 7 ? sent : refused) += 1;
  }
  CHECK(sent > 20 && refused > 50);
  while (peer->receive_from_ip(ByteView(buffer, sizeof(buffer)), &from, &from_port, &length)) {}
}

// --- AF_NETLINK ---------------------------------------------------------------------------------------------------------------------------------------------
namespace nl {
struct Message { uint16_t type = 0, flags = 0; uint32_t sequence = 0, port = 0; std::string payload; };
uint32_t word_at(const std::string& b, size_t at) { uint32_t v = 0; for (int i = 3; i >= 0; --i) v = (v << 8) | uint8_t(b[at + size_t(i)]); return v; }
uint16_t half_at(const std::string& b, size_t at) { return uint16_t(uint8_t(b[at]) | uint8_t(b[at + 1]) << 8); }
void put(std::string* out, uint64_t v, int bytes) { for (int i = 0; i < bytes; ++i) out->push_back(char(v >> (8 * i))); }
// one request: a header and a payload, padded
std::string request(uint16_t type, uint16_t flags, uint32_t sequence, const std::string& payload) {
  std::string out;
  put(&out, 16 + payload.size(), 4); put(&out, type, 2); put(&out, flags, 2); put(&out, sequence, 4); put(&out, 0, 4);
  out += payload;
  while (out.size() % 4 != 0) out.push_back('\0');
  return out;
}
std::string attribute(uint16_t type, const std::string& data) {
  std::string out;
  put(&out, 4 + data.size(), 2); put(&out, type, 2);
  out += data;
  while (out.size() % 4 != 0) out.push_back('\0');
  return out;
}
// The messages of one datagram; *whole says whether the datagram is made of whole messages and nothing else.
std::vector<Message> messages(const std::string& datagram, bool* whole = nullptr) {
  std::vector<Message> out;
  size_t at = 0;
  while (at + 16 <= datagram.size()) {
    const uint32_t length = word_at(datagram, at);
    if (length < 16 || length > datagram.size() - at) break;
    Message m;
    m.type = half_at(datagram, at + 4); m.flags = half_at(datagram, at + 6); m.sequence = word_at(datagram, at + 8); m.port = word_at(datagram, at + 12);
    m.payload = datagram.substr(at + 16, length - 16);
    out.push_back(m);
    at += (size_t(length) + 3) & ~size_t{3};
  }
  if (whole) *whole = at == datagram.size();
  return out;
}
// The attributes after the first `skip` bytes of a payload, by type; *whole as above.
std::map<uint16_t, std::string> attributes(const std::string& payload, size_t skip, bool* whole = nullptr) {
  std::map<uint16_t, std::string> out;
  size_t at = skip;
  while (at + 4 <= payload.size()) {
    const uint16_t length = half_at(payload, at);
    if (length < 4 || length > payload.size() - at) break;
    out[half_at(payload, at + 2)] = payload.substr(at + 4, size_t(length) - 4);
    at += (size_t(length) + 3) & ~size_t{3};
  }
  if (whole) *whole = at == payload.size();
  return out;
}
// The datagrams of a queue as the message code writes it (length, group, sender, bytes).
struct Queued { uint32_t group = 0, sender = 0; std::string bytes; };
std::vector<Queued> queued(const ByteBuffer& buffer, bool* whole) {
  const std::string all(reinterpret_cast<const char*>(buffer.view().data()), buffer.size());
  std::vector<Queued> out;
  size_t at = 0;
  while (at + 12 <= all.size()) {
    const uint32_t length = word_at(all, at);
    if (length > all.size() - at - 12) break;
    out.push_back(Queued{word_at(all, at + 4), word_at(all, at + 8), all.substr(at + 12, length)});
    at += 12 + size_t(length);
  }
  *whole = at == all.size();
  return out;
}
linuxabi::netlink::Snapshot example() {
  using namespace linuxabi::netlink;
  Snapshot n;
  Interface& lo = n.interfaces[n.count++];
  lo.index = 1; lo.name[0] = 'l'; lo.name[1] = 'o'; lo.name_size = 2; lo.loopback = true; lo.mtu = 65536;
  Interface& e = n.interfaces[n.count++];
  e.index = 2; memcpy(e.name, "eth0", 4); e.name_size = 4;
  const uint8_t mac[6] = {2, 0, 0, 0, 0, 0xA1}, a4[4] = {192, 168, 5, 77}, g4[4] = {192, 168, 5, 1};
  memcpy(e.mac, mac, 6); memcpy(e.address4, a4, 4); memcpy(e.gateway4, g4, 4);
  e.has4 = true; e.prefix4 = 24;
  e.has_link6 = true; e.link6[0] = 0xFE; e.link6[1] = 0x80; e.link6[15] = 0xA1;
  e.has_global6 = true; e.global6[0] = 0x20; e.global6[1] = 0x01; e.global6[2] = 0x0D; e.global6[3] = 0xB8; e.global6[15] = 0xA1;
  e.has_router6 = true; e.router6[0] = 0xFE; e.router6[1] = 0x80; e.router6[15] = 1;
  return n;
}
}  // namespace nl

// The kernel's side of a netlink socket on its own: requests in, datagrams out.
static void test_netlink_messages() {
  using namespace linuxabi::netlink;
  constexpr uint16_t kRequest = 1, kMulti = 2, kAck = 4, kDump = 0x300;
  Lan lan;      // (for its allocator)
  const Snapshot network = nl::example();
  Asker asker;
  asker.port = 4242;
  auto ask = [&](const std::string& request, const Asker& who, bool* whole) {
    ByteBuffer out(lan.alloc);
    CHECK(answer(who, ConstByteView(reinterpret_cast<const uint8_t*>(request.data()), request.size()), network, &out));
    return nl::queued(out, whole);
  };
  bool whole = false, inner = false;
  // every interface: glibc's getifaddrs asks with a one-byte struct rtgenmsg
  std::vector<nl::Queued> d = ask(nl::request(18, kRequest | kDump, 7, std::string(1, '\0')), asker, &whole);
  CHECK(whole && d.size() == 2);
  if (d.size() == 2) {
    std::vector<nl::Message> m = nl::messages(d[0].bytes, &inner);
    CHECK(inner && m.size() == 2 && d[0].group == 0 && d[0].sender == 0);
    if (m.size() == 2) {
      CHECK(m[0].type == 16 && m[0].flags == kMulti && m[0].sequence == 7 && m[0].port == 4242 && m[0].payload.size() > 16);
      std::map<uint16_t, std::string> a = nl::attributes(m[0].payload, 16, &inner);
      CHECK(inner && nl::half_at(m[0].payload, 2) == 772 && nl::word_at(m[0].payload, 4) == 1 && (nl::word_at(m[0].payload, 8) & 0x49) == 0x49);
      CHECK(a[3] == std::string("lo\0", 3) && a[4] == std::string("\0\0\x01\0", 4) && a[1] == std::string(6, '\0') && a[7].size() == 96 && a[23].size() == 200);
      a = nl::attributes(m[1].payload, 16, &inner);
      CHECK(inner && nl::half_at(m[1].payload, 2) == 1 && nl::word_at(m[1].payload, 4) == 2 && (nl::word_at(m[1].payload, 8) & 0x1043) == 0x1043);
      CHECK(a[3] == std::string("eth0\0", 5) && a[1] == std::string("\x02\0\0\0\0\xA1", 6) && a[2] == std::string(6, '\xFF') && a[4] == std::string("\xDC\x05\0\0", 4));
    }
    m = nl::messages(d[1].bytes, &inner);
    CHECK(inner && m.size() == 1 && m[0].type == 3 && m[0].flags == kMulti && m[0].sequence == 7 && m[0].payload == std::string(4, '\0'));
  }
  // every address, IPv4 ones first; only one family when asked
  d = ask(nl::request(22, kRequest | kDump, 8, std::string(1, '\0')), asker, &whole);
  CHECK(whole && d.size() == 2);
  if (d.size() == 2) {
    const std::vector<nl::Message> m = nl::messages(d[0].bytes, &inner);
    CHECK(inner && m.size() == 5);
    if (m.size() == 5) {
      std::map<uint16_t, std::string> a = nl::attributes(m[0].payload, 8, &inner);      // 127.0.0.1/8 on lo, scope host
      CHECK(inner && m[0].type == 20 && m[0].payload.substr(0, 8) == std::string("\x02\x08\x80\xFE\x01\0\0\0", 8) && a[1] == std::string("\x7F\0\0\x01", 4) && a[2] == a[1] && a[3] == std::string("lo\0", 3));
      a = nl::attributes(m[1].payload, 8, &inner);                                      // 192.168.5.77/24 on eth0, with its broadcast address
      CHECK(inner && m[1].payload.substr(0, 8) == std::string("\x02\x18\0\0\x02\0\0\0", 8) && a[1] == std::string("\xC0\xA8\x05\x4D", 4) && a[2] == a[1] && a[4] == std::string("\xC0\xA8\x05\xFF", 4) &&
            a[3] == std::string("eth0\0", 5) && a[6].size() == 16);
      a = nl::attributes(m[2].payload, 8, &inner);                                      // ::1/128
      CHECK(inner && m[2].payload.substr(0, 8) == std::string("\x0A\x80\x80\xFE\x01\0\0\0", 8) && a[1] == std::string(15, '\0') + '\x01' && a.count(2) == 0);
      a = nl::attributes(m[3].payload, 8, &inner);                                      // the global address, then the link-local one (scope link)
      CHECK(inner && m[3].payload.substr(0, 8) == std::string("\x0A\x40\0\0\x02\0\0\0", 8) && a[1].size() == 16 && a[1][0] == 0x20);
      CHECK(m[4].payload.substr(0, 8) == std::string("\x0A\x40\x80\xFD\x02\0\0\0", 8));
    }
  }
  d = ask(nl::request(22, kRequest | kDump, 9, std::string("\x0A\0\0\0\0\0\0\0", 8)), asker, &whole);
  CHECK(whole && d.size() == 2 && nl::messages(d[0].bytes).size() == 3);
  // the routes: the default one and the subnet; IPv6's
  d = ask(nl::request(26, kRequest | kDump, 10, std::string("\x02\0\0\0\0\0\0\0\0\0\0\0", 12)), asker, &whole);
  CHECK(whole && d.size() == 2);
  if (d.size() == 2) {
    const std::vector<nl::Message> m = nl::messages(d[0].bytes, &inner);
    CHECK(inner && m.size() == 2);
    if (m.size() == 2) {
      std::map<uint16_t, std::string> a = nl::attributes(m[0].payload, 12, &inner);
      CHECK(inner && m[0].type == 24 && m[0].payload.substr(0, 8) == std::string("\x02\0\0\0\xFE\x10\0\x01", 8) && a[5] == std::string("\xC0\xA8\x05\x01", 4) && a[4] == std::string("\x02\0\0\0", 4) && a.count(1) == 0);
      a = nl::attributes(m[1].payload, 12, &inner);
      CHECK(inner && m[1].payload.substr(0, 8) == std::string("\x02\x18\0\0\xFE\x02\xFD\x01", 8) && a[1] == std::string("\xC0\xA8\x05\0", 4) && a[7] == std::string("\xC0\xA8\x05\x4D", 4) && a.count(5) == 0);
    }
  }
  d = ask(nl::request(26, kRequest | kDump, 11, std::string("\x0A", 1)), asker, &whole);
  CHECK(whole && d.size() == 2 && nl::messages(d[0].bytes).size() == 4);
  // the route to one place: by the gateway, on the link, to the machine itself; nowhere
  auto route_to = [&](const std::string& head, const std::string& to) { return ask(nl::request(26, kRequest, 12, head + nl::attribute(1, to)), asker, &whole); };
  d = route_to(std::string("\x02\x20\0\0\0\0\0\0\0\0\0\0", 12), std::string("\x08\x08\x08\x08", 4));
  CHECK(whole && d.size() == 1);
  if (d.size() == 1) {
    const std::vector<nl::Message> m = nl::messages(d[0].bytes, &inner);
    std::map<uint16_t, std::string> a = nl::attributes(m.empty() ? std::string() : m[0].payload, 12);
    CHECK(inner && m.size() == 1 && m[0].type == 24 && m[0].flags == 0 && a[1] == std::string("\x08\x08\x08\x08", 4) && a[5] == std::string("\xC0\xA8\x05\x01", 4) && a[7] == std::string("\xC0\xA8\x05\x4D", 4) &&
          a[4] == std::string("\x02\0\0\0", 4));
  }
  d = route_to(std::string("\x02\x20\0\0\0\0\0\0\0\0\0\0", 12), std::string("\xC0\xA8\x05\x09", 4));
  CHECK(d.size() == 1 && nl::attributes(nl::messages(d[0].bytes)[0].payload, 12).count(5) == 0);
  d = route_to(std::string("\x02\x20\0\0\0\0\0\0\0\0\0\0", 12), std::string("\x7F\0\0\x05", 4));
  CHECK(d.size() == 1 && nl::attributes(nl::messages(d[0].bytes)[0].payload, 12)[4] == std::string("\x01\0\0\0", 4) && nl::messages(d[0].bytes)[0].payload[7] == 2);
  d = route_to(std::string("\x0A\x80\0\0\0\0\0\0\0\0\0\0", 12), std::string("\x20\x01\x48\x60", 4) + std::string(12, '\x01'));
  CHECK(d.size() == 1 && nl::attributes(nl::messages(d[0].bytes)[0].payload, 12)[5].size() == 16 && nl::attributes(nl::messages(d[0].bytes)[0].payload, 12)[7][0] == 0x20);
  {
    Snapshot alone;
    alone.interfaces[0] = network.interfaces[0];
    alone.count = 1;
    ByteBuffer out(lan.alloc);
    const std::string r = nl::request(26, kRequest, 13, std::string("\x02\x20\0\0\0\0\0\0\0\0\0\0", 12) + nl::attribute(1, std::string("\x08\x08\x08\x08", 4)));
    CHECK(answer(asker, ConstByteView(reinterpret_cast<const uint8_t*>(r.data()), r.size()), alone, &out));
    const std::vector<nl::Queued> q = nl::queued(out, &whole);
    CHECK(q.size() == 1 && nl::messages(q[0].bytes)[0].type == 2 && int32_t(nl::word_at(nl::messages(q[0].bytes)[0].payload, 0)) == -101);      // ENETUNREACH
  }
  // one interface by number and by name; one there is not
  d = ask(nl::request(18, kRequest, 14, std::string("\0\0\0\0\x02\0\0\0\0\0\0\0\0\0\0\0", 16)), asker, &whole);
  CHECK(whole && d.size() == 1 && nl::messages(d[0].bytes).size() == 1 && nl::attributes(nl::messages(d[0].bytes)[0].payload, 16)[3] == std::string("eth0\0", 5));
  d = ask(nl::request(18, kRequest | kAck, 15, std::string(16, '\0') + nl::attribute(3, std::string("lo\0", 3))), asker, &whole);
  CHECK(whole && d.size() == 2 && nl::word_at(nl::messages(d[0].bytes)[0].payload, 4) == 1 && nl::messages(d[1].bytes)[0].type == 2 && nl::messages(d[1].bytes)[0].payload.size() == 20 &&
        nl::word_at(nl::messages(d[1].bytes)[0].payload, 0) == 0);      // (asked to acknowledge: an error message that says 0)
  const std::string missing = nl::request(18, kRequest, 16, std::string("\0\0\0\0\x09\0\0\0\0\0\0\0\0\0\0\0", 16));
  d = ask(missing, asker, &whole);
  CHECK(whole && d.size() == 1);
  if (d.size() == 1) {      // the error and the whole request it is for; with NETLINK_CAP_ACK only its header
    const std::vector<nl::Message> m = nl::messages(d[0].bytes, &inner);
    CHECK(inner && m.size() == 1 && m[0].type == 2 && m[0].sequence == 16 && int32_t(nl::word_at(m[0].payload, 0)) == -19 && m[0].payload.substr(4) == missing);
  }
  Asker capped = asker;
  capped.cap_ack = true;
  d = ask(missing, capped, &whole);
  CHECK(d.size() == 1 && nl::messages(d[0].bytes)[0].payload.size() == 20 && nl::messages(d[0].bytes)[0].flags == 0x100);
  // what is not a Linux program's to do, what nobody knows, what is not a request, and several requests in one datagram
  d = ask(nl::request(16, kRequest, 17, std::string(16, '\0')), asker, &whole);
  CHECK(d.size() == 1 && int32_t(nl::word_at(nl::messages(d[0].bytes)[0].payload, 0)) == -1);
  d = ask(nl::request(200, kRequest, 18, std::string(16, '\0')), asker, &whole);
  CHECK(d.size() == 1 && int32_t(nl::word_at(nl::messages(d[0].bytes)[0].payload, 0)) == -95);
  d = ask(nl::request(18, kDump, 19, std::string(1, '\0')), asker, &whole);
  CHECK(whole && d.empty());
  d = ask(nl::request(30, kRequest | kDump, 20, std::string(1, '\0')), asker, &whole);      // neighbours: none
  CHECK(d.size() == 1 && nl::messages(d[0].bytes)[0].type == 3);
  d = ask(nl::request(1, kRequest | kAck, 21, "") + nl::request(22, kRequest | kDump, 22, std::string(1, '\x02')) + nl::request(18, kRequest | kDump, 23, std::string(1, '\0')), asker, &whole);
  CHECK(whole && d.size() == 5 && nl::messages(d[0].bytes)[0].type == 2 && nl::messages(d[0].bytes)[0].sequence == 21 && nl::messages(d[2].bytes)[0].sequence == 22 &&
        nl::messages(d[4].bytes)[0].sequence == 23);
  // the generic controller knows itself and nothing else
  Asker generic = asker;
  generic.protocol = kProtocolGeneric;
  d = ask(nl::request(0x10, kRequest, 30, std::string("\x03\x01\0\0", 4) + nl::attribute(2, std::string("nlctrl\0", 7))), generic, &whole);
  CHECK(whole && d.size() == 1 && nl::messages(d[0].bytes)[0].type == 0x10 && nl::attributes(nl::messages(d[0].bytes)[0].payload, 4)[1] == std::string("\x10\0", 2));
  d = ask(nl::request(0x10, kRequest, 31, std::string("\x03\x01\0\0", 4) + nl::attribute(2, std::string("nl80211\0", 8))), generic, &whole);
  CHECK(d.size() == 1 && nl::messages(d[0].bytes)[0].type == 2 && int32_t(nl::word_at(nl::messages(d[0].bytes)[0].payload, 0)) == -2);
  d = ask(nl::request(0x10, kRequest | kDump, 32, std::string("\x03\x01\0\0", 4)), generic, &whole);
  CHECK(whole && d.size() == 2);
  // many interfaces: a dump goes out in several datagrams, none large
  {
    Snapshot many;
    for (uint32_t i = 0; i < kMostInterfaces; ++i) {
      many.interfaces[i] = network.interfaces[1];
      many.interfaces[i].index = i + 1;
    }
    many.count = kMostInterfaces;
    ByteBuffer out(lan.alloc);
    const std::string r = nl::request(18, kRequest | kDump, 40, std::string(1, '\0'));
    CHECK(answer(asker, ConstByteView(reinterpret_cast<const uint8_t*>(r.data()), r.size()), many, &out));
    const std::vector<nl::Queued> q = nl::queued(out, &whole);
    size_t links = 0;
    for (const nl::Queued& one : q) {
      CHECK(one.bytes.size() <= 4096);
      for (const nl::Message& m : nl::messages(one.bytes)) links += m.type == 16;
    }
    CHECK(whole && q.size() >= 3 && links == kMostInterfaces);
  }
  // what changed between two states of the network, for the groups a socket listens to
  {
    Snapshot later = network;
    later.interfaces[1].address4[3] = 78;
    later.interfaces[1].has_global6 = false;
    ByteBuffer out(lan.alloc);
    constexpr uint32_t kLinks = 1, kAddresses4 = 0x10, kRoutes4 = 0x40, kAddresses6 = 0x100;
    CHECK(changes(network, later, kAddresses4 | kAddresses6, &out));
    std::vector<nl::Queued> q = nl::queued(out, &whole);
    CHECK(whole && q.size() == 3);
    if (q.size() == 3) {
      CHECK(q[0].group == 5 && nl::messages(q[0].bytes)[0].type == 21 && nl::attributes(nl::messages(q[0].bytes)[0].payload, 8)[1] == std::string("\xC0\xA8\x05\x4D", 4));
      CHECK(q[1].group == 5 && nl::messages(q[1].bytes)[0].type == 20 && nl::attributes(nl::messages(q[1].bytes)[0].payload, 8)[1] == std::string("\xC0\xA8\x05\x4E", 4));
      CHECK(q[2].group == 9 && nl::messages(q[2].bytes)[0].type == 21 && nl::messages(q[2].bytes)[0].port == 0 && nl::messages(q[2].bytes)[0].sequence == 0);
    }
    out.clear();
    CHECK(changes(network, later, kRoutes4, &out));
    q = nl::queued(out, &whole);
    CHECK(whole && q.size() == 4 && q[0].group == 7);      // (both IPv4 routes name the address as their source)
    out.clear();
    CHECK(changes(network, network, 0xFFFFFFFF, &out) && out.size() == 0 && changes(network, later, kLinks, &out) && out.size() == 0);
    later.count = 1;
    CHECK(changes(network, later, kLinks, &out));
    q = nl::queued(out, &whole);
    CHECK(whole && q.size() == 1 && q[0].group == 1 && nl::messages(q[0].bytes)[0].type == 17);
  }
  // A datagram cut short anywhere, and with any bit of it flipped: answered or not, and what comes out is always whole datagrams of whole messages
  const std::string several = nl::request(18, kRequest | kDump, 50, std::string(1, '\0')) + nl::request(18, kRequest | kAck, 51, std::string(16, '\0') + nl::attribute(3, std::string("eth0\0", 5))) +
                              nl::request(26, kRequest, 52, std::string("\x02\x20\0\0\0\0\0\0\0\0\0\0", 12) + nl::attribute(1, std::string("\x08\x08\x08\x08", 4))) +
                              nl::request(22, kRequest | kDump, 53, std::string("\x0A\0\0\0\0\0\0\0", 8));
  size_t answers = 0;
  auto sound = [&](const std::string& request, const Asker& who) {
    bool all = false;
    const std::vector<nl::Queued> q = ask(request, who, &all);
    CHECK(all);
    for (const nl::Queued& one : q) {
      bool good = false;
      const std::vector<nl::Message> m = nl::messages(one.bytes, &good);
      CHECK(good && !m.empty());
      for (const nl::Message& each : m) CHECK(each.port == who.port);
      answers += m.size();
    }
  };
  for (size_t cut = 0; cut <= several.size(); ++cut) sound(several.substr(0, cut), asker);
  for (size_t bit = 0; bit < several.size() * 8; ++bit) {
    std::string flipped = several;
    flipped[bit / 8] = char(flipped[bit / 8] ^ (1 << (bit % 8)));
    sound(flipped, asker);
    if (bit < 60 * 8) sound(flipped, generic);
  }
  CHECK(answers > 5000);
}

// AF_NETLINK sockets through the system calls.
static void test_netlink_sockets() {
  constexpr uint64_t kNetlink = 16, kRaw = 3;
  constexpr uint16_t kRequest = 1, kDump = 0x300;
  Setup t;
  t.configure_ipv6();
  const int64_t fd = t.s(41, kNetlink, kRaw | kCloexec, 0);
  CHECK(fd >= 3 && t.s(41, kNetlink, kStream, 0) == -94 && t.s(41, kNetlink, kRaw, 9) == -93);      // (SOCK_STREAM; NETLINK_AUDIT)
  uint32_t v = 0;
  CHECK(t.option(fd, 1, 3, &v) == 0 && v == kRaw && t.option(fd, 1, 39, &v) == 0 && v == kNetlink && t.option(fd, 1, 38, &v) == 0 && v == 0);
  const uint64_t name = t.alloc(16), size = t.alloc(8), buffer = t.alloc(8192);
  auto address = [&](uint32_t port, uint32_t groups, uint16_t family = 16) {
    const uint64_t at = t.alloc(16);
    t.put32(at, family); t.put32(at + 4, port); t.put32(at + 8, groups);
    return at;
  };
  auto own = [&](int64_t socket, uint32_t* groups = nullptr) {
    t.put32(size, 12);
    if (t.s(51, socket, name, size) != 0 || t.u32(size) != 12 || (t.u32(name) & 0xFFFF) != 16) return ~uint32_t{0};
    if (groups) *groups = t.u32(name + 8);
    return t.u32(name + 4);
  };
  // not bound until it is used or bound; bound, its address is the process's number
  CHECK(own(fd) == 0 && t.polled(fd) == kPollOut && t.s(49, fd, address(0, 0), 12) == 0 && own(fd) == 7);
  CHECK(t.s(49, fd, address(99, 0), 12) == -22 && t.s(49, fd, address(0, 0), 8) == -22 && t.s(49, fd, address(0, 0, 2), 12) == -22 && t.s(49, fd, kBad, 12) == -14);
  auto send = [&](int64_t socket, const std::string& request) { return t.s(44, socket, t.bytes(request), request.size(), 0, 0, 0); };
  // everything one request brings, read datagram by datagram until NLMSG_DONE (or one that is not part of a list)
  auto exchange = [&](int64_t socket, const std::string& request) {
    std::vector<nl::Message> all;
    if (send(socket, request) != int64_t(request.size())) return all;
    for (int i = 0; i < 64; ++i) {
      const int64_t n = t.s(45, socket, buffer, 8192, 0x40 /* MSG_DONTWAIT */, 0, 0);
      if (n <= 0) break;
      bool whole = false;
      const std::vector<nl::Message> some = nl::messages(t.text(buffer, uint64_t(n)), &whole);
      CHECK(whole);
      bool last = false;
      for (const nl::Message& m : some) {
        all.push_back(m);
        last = last || m.type == 3 || !(m.flags & 2);
      }
      if (last) break;
    }
    return all;
  };
  // getifaddrs: the interfaces, then the addresses
  std::vector<nl::Message> m = exchange(fd, nl::request(18, kRequest | kDump, 1, std::string(1, '\0')));
  CHECK(m.size() == 3 && t.polled(fd) == kPollOut);
  if (m.size() == 3) {
    std::map<uint16_t, std::string> a = nl::attributes(m[1].payload, 16);
    CHECK(m[0].type == 16 && m[0].port == 7 && m[0].sequence == 1 && nl::attributes(m[0].payload, 16)[3] == std::string("lo\0", 3) && a[3] == std::string("eth0\0", 5) && nl::word_at(m[1].payload, 4) == 2);
    CHECK(a[1].size() == 6 && a[1] == std::string(reinterpret_cast<const char*>(t.lan.nic_a.mac.bytes), 6) && m[2].type == 3);
  }
  m = exchange(fd, nl::request(22, kRequest | kDump, 2, std::string(1, '\0')));
  CHECK(m.size() == 6);
  if (m.size() == 6) {
    const network::Ipv6Configuration c6 = t.lan.a.configuration6();
    CHECK(nl::attributes(m[1].payload, 8)[1] == std::string("\x0A\0\0\x01", 4) && m[1].payload[1] == 24 && nl::attributes(m[1].payload, 8)[4] == std::string("\x0A\0\0\xFF", 4));
    CHECK(nl::attributes(m[3].payload, 8)[1] == std::string(reinterpret_cast<const char*>(c6.global.bytes), 16) && m[3].payload[1] == 64);
    CHECK(nl::attributes(m[4].payload, 8)[1] == std::string(reinterpret_cast<const char*>(c6.link_local.bytes), 16) && uint8_t(m[4].payload[3]) == 253);
  }
  m = exchange(fd, nl::request(26, kRequest | kDump, 3, std::string("\x02", 1)));
  CHECK(m.size() == 3 && nl::attributes(m[0].payload, 12)[5] == std::string("\x0A\0\0\xFE", 4));
  // an error comes back as a message; a datagram can be looked at and sized before it is read; the sender is the kernel
  const std::string refused = nl::request(16, kRequest, 4, std::string(16, '\0'));
  CHECK(send(fd, refused) == int64_t(refused.size()) && t.polled(fd) == (kPollIn | kPollOut));
  CHECK(t.s(16, fd, 0x541B, size) == 0 && t.u32(size) == 36 + refused.size() - 16);
  CHECK(t.s(45, fd, buffer, 0, 0x22 /* MSG_PEEK | MSG_TRUNC */, 0, 0) == int64_t(20 + refused.size()) && t.s(45, fd, buffer, 16, 2, 0, 0) == 16);
  t.put32(size, 16); t.put32(name + 4, 77);
  CHECK(t.s(45, fd, buffer, 8192, 0, name, size) == int64_t(20 + refused.size()) && t.u32(size) == 12 && (t.u32(name) & 0xFFFF) == 16 && t.u32(name + 4) == 0 && t.u32(name + 8) == 0);
  m = nl::messages(t.text(buffer, 20 + refused.size()));
  CHECK(m.size() == 1 && m[0].type == 2 && int32_t(nl::word_at(m[0].payload, 0)) == -1 && t.s(45, fd, buffer, 64, 0x40, 0, 0) == -11);
  // sendmsg and recvmsg: several buffers, a datagram longer than them (MSG_TRUNC), who sent it (SO_PASSCRED) and to which group (NETLINK_PKTINFO)
  const uint64_t vectors = t.alloc(32), message = t.alloc(56), control = t.alloc(128), part = t.alloc(64);
  const std::string one = nl::request(18, kRequest, 5, std::string("\0\0\0\0\x01\0\0\0\0\0\0\0\0\0\0\0", 16));
  CHECK(t.s(54, fd, 1, 16, t.word(1), 4) == 0 && t.s(54, fd, 270, 3, t.word(1), 4) == 0 && t.option(fd, 1, 16, &v) == 0 && v == 1 && t.option(fd, 270, 3, &v) == 0 && v == 1);
  t.put64(vectors, t.bytes(one.substr(0, 10))); t.put64(vectors + 8, 10); t.put64(vectors + 16, t.bytes(one.substr(10))); t.put64(vectors + 24, one.size() - 10);
  t.put64(message, address(0, 0)); t.put32(message + 8, 12); t.put64(message + 16, vectors); t.put64(message + 24, 2); t.put64(message + 32, 0); t.put64(message + 40, 0); t.put32(message + 48, 0);
  CHECK(t.s(46, fd, message, 0) == int64_t(one.size()));
  t.put64(vectors, part); t.put64(vectors + 8, 16); t.put64(vectors + 16, part + 16); t.put64(vectors + 24, 16);
  t.put64(message, name); t.put32(message + 8, 16); t.put64(message + 32, control); t.put64(message + 40, 128);
  const int64_t whole_size = t.s(47, fd, message, 0x22);
  CHECK(whole_size > 200 && t.u32(message + 48) == 0x20 && t.u32(message + 8) == 12 && t.u64(message + 40) == 32 + 24);
  CHECK(t.text(control, 32) == std::string("\x1C\0\0\0\0\0\0\0\x01\0\0\0\x02\0\0\0", 16) + std::string(16, '\0'));                                    // SCM_CREDENTIALS: process 0, user 0, group 0
  CHECK(t.text(control + 32, 24) == std::string("\x14\0\0\0\0\0\0\0\x0E\x01\0\0\x01\0\0\0", 16) + std::string(8, '\0'));                               // NETLINK_PKTINFO: no group
  t.put64(message + 40, 128);
  CHECK(t.s(47, fd, message, 0) == 32 && nl::word_at(t.text(part, 32), 0) == uint32_t(whole_size) && t.s(47, fd, message, 0x40) == -11);      // (cut to the buffers; the rest is gone)
  CHECK(t.s(54, fd, 1, 16, t.word(0), 4) == 0 && t.s(54, fd, 270, 3, t.word(0), 4) == 0);
  // read and write, readv and writev
  CHECK(t.s(1, fd, t.bytes(one), one.size()) == int64_t(one.size()) && t.s(0, fd, buffer, 8192) == whole_size);
  t.put64(vectors, t.bytes(one)); t.put64(vectors + 8, one.size());
  CHECK(t.s(20, fd, vectors, 1) == int64_t(one.size()));
  t.put64(vectors, buffer); t.put64(vectors + 8, 8192);
  CHECK(t.s(19, fd, vectors, 1) == whole_size);
  // what a netlink socket is not
  CHECK(t.s(50, fd, 1) == -95 && t.s(43, fd, 0, 0) == -95 && t.s(48, fd, 2) == -95 && t.s(44, fd, buffer, 70000, 0, 0, 0) == -90 && t.s(44, fd, kBad, 32, 0, 0, 0) == -14);
  CHECK(t.s(54, fd, 6, 1, t.word(1), 4) == -92 && t.option(fd, 6, 1, &v) == -92 && t.s(54, fd, 270, 99, t.word(1), 4) == -92);
  // options: kept and read back; a filter is taken
  CHECK(t.s(54, fd, 270, 10, t.word(1), 4) == 0 && t.s(54, fd, 270, 11, t.word(1), 4) == 0 && t.s(54, fd, 270, 12, t.word(1), 4) == 0 && t.option(fd, 270, 10, &v) == 0 && v == 1 &&
        t.option(fd, 270, 11, &v) == 0 && v == 1 && t.option(fd, 270, 12, &v) == 0 && v == 1);
  CHECK(send(fd, nl::request(18, kRequest, 6, std::string("\0\0\0\0\x09\0\0\0\0\0\0\0\0\0\0\0", 16))) == 32 && t.s(45, fd, buffer, 8192, 0, 0, 0) == 36);      // (NETLINK_CAP_ACK: the header only)
  CHECK(t.s(54, fd, 1, 8, t.word(65536), 4) == 0 && t.option(fd, 1, 8, &v) == 0 && v == 131072 && t.s(54, fd, 1, 26, t.alloc(16), 16) == 0 && t.s(54, fd, 1, 27, 0, 0) == 0 &&
        t.s(54, fd, 1, 27, 0, 0) == -2);
  // a second socket of the process gets another address; an address in use; two sockets talk to each other
  const int64_t second = t.s(41, kNetlink, kRaw | kNonBlock, 0);
  CHECK(t.s(49, second, address(7, 0), 12) == -98 && t.s(49, second, address(0, 0), 12) == 0 && own(second) == 0xFFFFEFFFu);
  CHECK(t.s(44, fd, t.bytes("to the other"), 12, 0, address(0xFFFFEFFFu, 0), 12) == 12 && t.s(44, fd, t.bytes("to nobody"), 9, 0, address(555, 0), 12) == -111);
  t.put32(size, 12);
  CHECK(t.polled(second, kPollIn) == kPollIn && t.s(45, second, buffer, 64, 0, name, size) == 12 && t.text(buffer, 12) == "to the other" && t.u32(name + 4) == 7);
  CHECK(t.s(42, second, address(7, 0), 12) == 0 && t.s(1, second, t.bytes("back"), 4) == 4 && t.s(0, fd, buffer, 64) == 4 && t.text(buffer, 4) == "back");
  t.put32(size, 12);
  CHECK(t.s(52, second, name, size) == 0 && t.u32(name + 4) == 7 && t.s(42, second, address(0, 0, 0), 12) == 0 && t.s(0, second, buffer, 64) == -11);
  // listening for changes: nothing while the network stays as it is; a new address is told (the old one's end first), and a wait wakes for it
  const int64_t watcher = t.s(41, kNetlink, kRaw | kNonBlock, 0);
  uint32_t groups = 0;
  CHECK(t.s(49, watcher, address(0, 0x10), 12) == 0 && own(watcher, &groups) == 0xFFFFEFFEu && groups == 0x10 && t.polled(watcher, kPollIn) == 0);
  CHECK(t.s(54, watcher, 270, 1, t.word(9), 4) == 0 && own(watcher, &groups) != 0 && groups == 0x110 && t.option(watcher, 270, 9, &v) == 0 && v == 0x110);      // NETLINK_ADD_MEMBERSHIP: IPv6 addresses
  CHECK(t.s(54, watcher, 270, 2, t.word(9), 4) == 0 && t.s(54, watcher, 270, 1, t.word(0), 4) == -22 && t.s(0, watcher, buffer, 8192) == -11);
  network::IpConfiguration changed = t.lan.a.configuration();
  changed.address = network::Ipv4Address::make(10, 0, 0, 9);
  int polls = 0;
  t.host.meanwhile = [&] { if (++polls == 3) t.lan.a.configure(changed); };
  const uint64_t pollfd = t.alloc(8);
  t.put32(pollfd, uint32_t(watcher)); t.put32(pollfd + 4, kPollIn);
  CHECK(t.s(7, pollfd, 1, 5000) == 1 && polls >= 3);
  t.host.meanwhile = nullptr;
  t.put64(vectors, buffer); t.put64(vectors + 8, 8192);
  t.put64(message, name); t.put32(message + 8, 12); t.put64(message + 16, vectors); t.put64(message + 24, 1); t.put64(message + 32, 0); t.put64(message + 40, 0);
  int64_t n = t.s(47, watcher, message, 0);
  m = nl::messages(t.text(buffer, n > 0 ? uint64_t(n) : 0));
  CHECK(n > 0 && m.size() == 1 && m[0].type == 21 && m[0].port == 0 && nl::attributes(m[0].payload, 8)[1] == std::string("\x0A\0\0\x01", 4) && t.u32(name + 4) == 0 && t.u32(name + 8) == 0x10);
  n = t.s(0, watcher, buffer, 8192);
  m = nl::messages(t.text(buffer, n > 0 ? uint64_t(n) : 0));
  CHECK(n > 0 && m.size() == 1 && m[0].type == 20 && nl::attributes(m[0].payload, 8)[1] == std::string("\x0A\0\0\x09", 4) && t.s(0, watcher, buffer, 8192) == -11);
  m = exchange(watcher, nl::request(22, kRequest | kDump, 9, std::string(1, '\x02')));      // (and it can ask like any other)
  CHECK(m.size() == 3 && nl::attributes(m[1].payload, 8)[1] == std::string("\x0A\0\0\x09", 4));
  // the generic controller; device events (a socket that hears nothing and that the kernel does not listen on)
  const int64_t generic = t.s(41, kNetlink, kRaw, 16), events = t.s(41, kNetlink, kDgram | kNonBlock, 15);
  m = exchange(generic, nl::request(0x10, kRequest, 1, std::string("\x03\x01\0\0", 4) + nl::attribute(2, std::string("nl80211\0", 8))));
  CHECK(m.size() == 1 && m[0].type == 2 && int32_t(nl::word_at(m[0].payload, 0)) == -2);
  CHECK(events > 0 && t.s(49, events, address(0, 1), 12) == 0 && t.polled(events, kPollIn) == 0 && t.s(0, events, buffer, 64) == -11 && send(events, std::string(16, 'x')) == -111);
  // more than the queue holds is lost, and the next read says so once
  const std::string dump = nl::request(18, kRequest | kDump, 60, std::string(1, '\0'));
  for (int i = 0; i < 800; ++i) {
    t.reuse();
    CHECK(send(fd, dump) == int64_t(dump.size()));
  }
  t.reuse();
  const uint64_t spare = t.alloc(8192);
  CHECK(t.s(45, fd, spare, 8192, 0x40, 0, 0) == -105);
  int read = 0;
  while (t.s(45, fd, spare, 8192, 0x40, 0, 0) > 0) ++read;
  CHECK(read > 200 && t.s(45, fd, spare, 8192, 0x40, 0, 0) == -11);
  // the interface ioctls, of any socket: a name for a number and back, flags, addresses, the list
  {
    t.reuse();
    const uint64_t ifreq = t.alloc(40), conf = t.alloc(16), list = t.alloc(160);
    const int64_t any = t.s(41, kAfInet, kDgram, 0);
    auto named = [&](const char* text) { memset(&t.memory.bytes[ifreq - kBase], 0, 40); t.memory.write(ifreq, ConstByteView(reinterpret_cast<const uint8_t*>(text), strlen(text))); };
    named("eth0");
    CHECK(t.s(16, any, 0x8933, ifreq) == 0 && t.u32(ifreq + 16) == 2 && t.s(16, fd, 0x8933, ifreq) == 0);                                     // SIOCGIFINDEX
    named("eth");
    CHECK(t.s(16, any, 0x8933, ifreq) == -19);
    named("");
    t.put32(ifreq + 16, 1);
    CHECK(t.s(16, any, 0x8910, ifreq) == 0 && t.text(ifreq, 3) == std::string("lo\0", 3));                                                    // SIOCGIFNAME
    t.put32(ifreq + 16, 5);
    CHECK(t.s(16, any, 0x8910, ifreq) == -19);
    named("eth0");
    CHECK(t.s(16, any, 0x8913, ifreq) == 0 && (t.u32(ifreq + 16) & 0xFFFF) == 0x1043 && t.text(ifreq, 5) == std::string("eth0\0", 5));         // SIOCGIFFLAGS
    CHECK(t.s(16, any, 0x8915, ifreq) == 0 && t.text(ifreq + 16, 8) == std::string("\x02\0\0\0\x0A\0\0\x09", 8));                             // SIOCGIFADDR
    CHECK(t.s(16, any, 0x891B, ifreq) == 0 && t.text(ifreq + 20, 4) == std::string("\xFF\xFF\xFF\0", 4));                                     // SIOCGIFNETMASK
    CHECK(t.s(16, any, 0x8919, ifreq) == 0 && t.text(ifreq + 20, 4) == std::string("\x0A\0\0\xFF", 4));                                       // SIOCGIFBRDADDR
    CHECK(t.s(16, any, 0x8921, ifreq) == 0 && t.u32(ifreq + 16) == 1500);                                                                     // SIOCGIFMTU
    CHECK(t.s(16, any, 0x8927, ifreq) == 0 && t.text(ifreq + 16, 8) == std::string("\x01\0", 2) + std::string(reinterpret_cast<const char*>(t.lan.nic_a.mac.bytes), 6));      // SIOCGIFHWADDR
    CHECK(t.s(16, any, 0x8914, ifreq) == -1 && t.s(16, any, 0x8933, kBad) == -14);                                                            // SIOCSIFFLAGS
    t.put32(conf, 0); t.put64(conf + 8, 0);
    CHECK(t.s(16, any, 0x8912, conf) == 0 && t.u32(conf) == 80);                                                                              // SIOCGIFCONF: how much there is
    t.put32(conf, 160); t.put64(conf + 8, list);
    CHECK(t.s(16, any, 0x8912, conf) == 0 && t.u32(conf) == 80 && t.text(list, 3) == std::string("lo\0", 3) && t.text(list + 20, 4) == std::string("\x7F\0\0\x01", 4) &&
          t.text(list + 40, 5) == std::string("eth0\0", 5) && t.text(list + 60, 4) == std::string("\x0A\0\0\x09", 4));
    t.put32(conf, 50);
    CHECK(t.s(16, any, 0x8912, conf) == 0 && t.u32(conf) == 40 && t.s(3, any) == 0);
  }
  // closed sockets are free again: many more than there can be at once
  CHECK(t.s(3, second) == 0 && t.s(3, watcher) == 0 && t.s(3, generic) == 0 && t.s(3, events) == 0);
  for (int i = 0; i < 200; ++i) {
    const int64_t again = t.s(41, kNetlink, kRaw, 0);
    CHECK(again >= 3 && t.s(49, again, address(0, 0), 12) == 0 && own(again) == 0xFFFFEFFFu && t.s(3, again) == 0);
  }
  CHECK(t.s(3, fd) == 0 && t.s(45, fd, spare, 64, 0, 0, 0) == -9);
}

// --- raw and ICMP sockets -----------------------------------------------------------------------------------------------------------------------------------
namespace raw {
uint16_t checksum(const std::string& bytes) {
  uint32_t sum = 0;
  for (size_t i = 0; i < bytes.size(); i += 2) sum += uint32_t(uint8_t(bytes[i])) << 8 | (i + 1 < bytes.size() ? uint8_t(bytes[i + 1]) : 0);
  while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
  return uint16_t(~sum);
}
// an ICMP message: type, code, checksum (computed unless `sum` is false), identifier, sequence, data
std::string icmp(uint8_t type, uint16_t identifier, uint16_t sequence, const std::string& data, bool sum = true) {
  std::string m;
  m.push_back(char(type)); m.push_back(0); m.push_back(0); m.push_back(0);
  m.push_back(char(identifier >> 8)); m.push_back(char(identifier)); m.push_back(char(sequence >> 8)); m.push_back(char(sequence));
  m += data;
  if (sum) { const uint16_t c = checksum(m); m[2] = char(c >> 8); m[3] = char(c); }
  return m;
}
uint16_t be16(const std::string& b, size_t at) { return uint16_t(uint8_t(b[at]) << 8 | uint8_t(b[at + 1])); }
}  // namespace raw

static void test_raw_sockets() {
  constexpr uint64_t kRaw = 3;
  Setup t;
  t.configure_ipv6();
  const network::Ipv6Configuration a6 = t.lan.a.configuration6(), b6 = t.lan.b.configuration6();
  const uint64_t buffer = t.alloc(2048), name = t.alloc(32), size = t.alloc(8);
  uint32_t v = 0;
  auto send_to = [&](int64_t fd, const std::string& bytes, uint64_t to, uint64_t to_size) { return t.s(44, fd, t.bytes(bytes), bytes.size(), 0, to, to_size); };
  // what one datagram of a socket is, once there is one (empty: none came)
  auto receive = [&](int64_t fd, std::string* from = nullptr, uint64_t name_size = 16) {
    if (!t.lan.run_until([&] { return t.polled(fd, kPollIn) == kPollIn; }, 3000)) return std::string();
    t.put32(size, uint32_t(name_size));
    const int64_t n = t.s(45, fd, buffer, 2048, 0, name, size);
    if (from) *from = t.text(name, t.u32(size));
    return n > 0 ? t.text(buffer, uint64_t(n)) : std::string();
  };
  auto quiet = [&](int64_t fd) { t.lan.step(30); return t.polled(fd, kPollIn) == 0; };
  // what there is and is not
  CHECK(t.s(41, kAfInet, kRaw, 0) == -93 && t.s(41, kAfInet, kDgram, 58) == -93 && t.s(41, kAfInet6, kDgram, 1) == -93 && t.s(41, kAfInet6, kRaw, 255) == -93 && t.s(41, kAfInet, kStream, 1) == -93);

  // An echo socket (SOCK_DGRAM, IPPROTO_ICMP): requests go out under the socket's identifier, whatever the program wrote; the reply comes back alone
  const int64_t ping = t.s(41, kAfInet, kDgram | kNonBlock, 1);
  CHECK(ping >= 3 && t.option(ping, 1, 3, &v) == 0 && v == kDgram && t.option(ping, 1, 38, &v) == 0 && v == 1 && t.polled(ping) == kPollOut);
  const std::string request = raw::icmp(8, 0x1234, 7, "are you there?", false);
  CHECK(send_to(ping, request, t.in4(10, 0, 0, 2, 0), 16) == int64_t(request.size()));
  t.put32(size, 16);
  CHECK(t.s(51, ping, name, size) == 0);
  const uint16_t identifier = raw::be16(t.text(name, 4), 2);
  std::string from, reply = receive(ping, &from);
  CHECK(identifier != 0 && identifier != 0x1234 && reply.size() == request.size() && reply[0] == 0 && raw::be16(reply, 4) == identifier && raw::be16(reply, 6) == 7 &&
        reply.substr(8) == "are you there?" && raw::checksum(reply) == 0 && from.substr(0, 8) == std::string("\x02\0\0\0\x0A\0\0\x02", 8));
  // the machine itself answers too; a connected one needs no address; the reply can be looked at first
  CHECK(send_to(ping, raw::icmp(8, 0, 8, "self"), t.in4(127, 0, 0, 1, 0), 16) == 12);
  reply = receive(ping, &from);
  CHECK(reply.size() == 12 && reply[0] == 0 && raw::be16(reply, 6) == 8 && from.substr(4, 4) == std::string("\x7F\0\0\x01", 4));
  CHECK(t.s(42, ping, t.in4(10, 0, 0, 2, 0), 16) == 0 && t.s(1, ping, t.bytes(raw::icmp(8, 0, 9, "connected")), 17) == 17 &&
        t.lan.run_until([&] { return t.polled(ping, kPollIn) == kPollIn; }, 3000) && t.s(45, ping, buffer, 4, 0x22 /* MSG_PEEK | MSG_TRUNC */, 0, 0) == 17 && t.s(0, ping, buffer, 64) == 17);
  // what is not an echo request; a second socket's own identifier, and one that is taken
  CHECK(send_to(ping, raw::icmp(0, 0, 1, "reply"), t.in4(10, 0, 0, 2, 0), 16) == -22 && send_to(ping, "\x08\0\0", t.in4(10, 0, 0, 2, 0), 16) == -22 &&
        send_to(ping, std::string(1481, '\x08'), t.in4(10, 0, 0, 2, 0), 16) == -90 && t.s(54, ping, 0, 3, t.word(1), 4) == -92);
  const int64_t other = t.s(41, kAfInet, kDgram | kNonBlock, 1), third = t.s(41, kAfInet, kDgram, 1);
  CHECK(t.s(49, other, t.in4(0, 0, 0, 0, 777), 16) == 0 && t.s(49, third, t.in4(0, 0, 0, 0, 777), 16) == -98);
  t.put32(size, 16);
  CHECK(t.s(51, other, name, size) == 0 && raw::be16(t.text(name, 4), 2) == 777 && send_to(other, raw::icmp(8, 1, 1, "mine"), t.in4(10, 0, 0, 2, 0), 16) == 12);
  reply = receive(other);
  CHECK(reply.size() == 12 && raw::be16(reply, 4) == 777 && quiet(ping));      // (and the first socket hears nothing of it)
  CHECK(t.s(3, third) == 0 && t.s(3, other) == 0);

  // A raw ICMP socket: what it sends is the ICMP message as the program made it, what it gets is whole packets, IP header first
  const int64_t icmp = t.s(41, kAfInet, kRaw | kNonBlock, 1);
  CHECK(icmp >= 3 && t.option(icmp, 1, 3, &v) == 0 && v == kRaw && t.option(icmp, 1, 38, &v) == 0 && v == 1 && t.option(icmp, 0, 2, &v) == 0 && v == 64);
  SharedPointer<network::IRawSocket> watcher = t.lan.b.raw_open(false, 1, network::RawKind::Packets, 0);      // (the other machine, listening to what arrives)
  uint8_t seen[1500];
  network::IpAddress seen_from;
  uint16_t seen_port = 0;
  uint32_t seen_length = 0;
  CHECK(watcher && send_to(icmp, raw::icmp(8, 0x5151, 1, "raw request"), t.in4(10, 0, 0, 2, 0), 16) == 19);
  std::string packet = receive(icmp, &from);
  CHECK(packet.size() == 20 + 19 && packet[0] == 0x45 && packet[9] == 1 && packet.substr(12, 8) == std::string("\x0A\0\0\x02\x0A\0\0\x01", 8) && raw::be16(packet, 2) == 39 &&
        packet[20] == 0 && raw::be16(packet, 24) == 0x5151 && packet.substr(28) == "raw request" && from.substr(0, 8) == std::string("\x02\0\0\0\x0A\0\0\x02", 8) && quiet(ping));
  CHECK(watcher->receive_from_ip(ByteView(seen), &seen_from, &seen_port, &seen_length) && seen_length == 39 && seen[8] == 64 && seen[20] == 8);      // (the request as it arrived there: TTL 64)
  // IP_TTL: how far its packets may go
  CHECK(t.s(54, icmp, 0, 2, t.word(3), 4) == 0 && t.option(icmp, 0, 2, &v) == 0 && v == 3 && t.s(54, icmp, 0, 2, t.word(0), 4) == -22 && t.s(54, icmp, 0, 2, t.word(256), 4) == -22);
  CHECK(send_to(icmp, raw::icmp(8, 0x5151, 2, "short lived"), t.in4(10, 0, 0, 2, 0), 16) == 19 && !receive(icmp).empty());
  CHECK(watcher->receive_from_ip(ByteView(seen), &seen_from, &seen_port, &seen_length) && seen[8] == 3);
  CHECK(t.s(54, icmp, 0, 2, t.word(0xFFFFFFFFu), 4) == 0 && t.option(icmp, 0, 2, &v) == 0 && v == 64);
  // over the loopback it hears its own request and the answer; ICMP_FILTER keeps the requests away
  CHECK(send_to(icmp, raw::icmp(8, 0x5151, 3, "loop"), t.in4(127, 0, 0, 1, 0), 16) == 12);
  packet = receive(icmp);
  std::string second = receive(icmp);
  CHECK(packet.size() == 32 && second.size() == 32 && packet[20] == 8 && second[20] == 0 && packet.substr(12, 4) == std::string("\x7F\0\0\x01", 4));
  CHECK(t.s(54, icmp, 255, 1, t.word(1u << 8), 4) == 0 && t.option(icmp, 255, 1, &v) == 0 && v == 0x100 && send_to(icmp, raw::icmp(8, 0x5151, 4, "loop"), t.in4(127, 0, 0, 1, 0), 16) == 12);
  packet = receive(icmp);
  CHECK(packet.size() == 32 && packet[20] == 0 && quiet(icmp));
  CHECK(t.s(54, icmp, 255, 1, t.word(0), 4) == 0 && t.s(54, icmp, 255, 1, t.word(0), 2) == -22 && t.s(54, ping, 255, 1, t.word(0), 4) == -92);
  // recvmsg says where it arrived (IP_PKTINFO), as for any datagram
  {
    const uint64_t vectors = t.alloc(16), message = t.alloc(56), control = t.alloc(64);
    CHECK(t.s(54, icmp, 0, 8, t.word(1), 4) == 0 && send_to(icmp, raw::icmp(8, 0x5151, 5, "info"), t.in4(10, 0, 0, 2, 0), 16) == 12 &&
          t.lan.run_until([&] { return t.polled(icmp, kPollIn) == kPollIn; }, 3000));
    t.put64(vectors, buffer); t.put64(vectors + 8, 2048);
    t.put64(message, name); t.put32(message + 8, 16); t.put64(message + 16, vectors); t.put64(message + 24, 1); t.put64(message + 32, control); t.put64(message + 40, 64);
    CHECK(t.s(47, icmp, message, 0) == 32 && t.u64(message + 40) == 32 && t.text(control + 16, 12) == std::string("\x02\0\0\0\x0A\0\0\x01\x0A\0\0\x01", 12));
  }
  // IP_HDRINCL: the program writes the whole packet; where it goes, what it carries and its TTL are taken from it. Here a UDP datagram, which the other
  // machine's UDP socket gets (the stack filled in the checksum).
  SharedPointer<network::IUdpSocket> peer = t.lan.b.udp_open_families(9950, network::IpFamilies::Both);
  SharedPointer<network::IRawSocket> udp_watcher = t.lan.b.raw_open(false, 17, network::RawKind::Packets, 0);
  const int64_t whole = t.s(41, kAfInet, kRaw, 17);
  std::string datagram("\x30\x39\x26\xDE\0\x0D\0\0hello", 13);      // from port 12345 to port 9950, 13 bytes, no checksum yet
  std::string header("\x45\0\0\0\0\0\0\0\x09\x11\0\0\0\0\0\0\x0A\0\0\x02", 20);
  CHECK(peer && udp_watcher && t.s(54, whole, 0, 3, t.word(1), 4) == 0 && t.option(whole, 0, 3, &v) == 0 && v == 1);
  CHECK(send_to(whole, header + datagram, t.in4(10, 0, 0, 2, 0), 16) == 33 && t.lan.run_until([&] { return peer->pending() > 0; }, 3000));
  CHECK(peer->receive_from_ip(ByteView(seen), &seen_from, &seen_port, &seen_length) && seen_length == 5 && seen_port == 12345 && memcmp(seen, "hello", 5) == 0);
  CHECK(udp_watcher->receive_from_ip(ByteView(seen), &seen_from, &seen_port, &seen_length) && seen_length == 33 && seen[8] == 9 && seen[9] == 17 && seen[12] == 10 && seen[15] == 1);
  CHECK(send_to(whole, header.substr(0, 12), t.in4(10, 0, 0, 2, 0), 16) == -22 && send_to(whole, std::string("\x65", 1) + header.substr(1) + datagram, t.in4(10, 0, 0, 2, 0), 16) == -22 &&
        send_to(whole, std::string("\x4F", 1) + header.substr(1) + datagram, t.in4(10, 0, 0, 2, 0), 16) == -22);
  // without it the socket's own protocol is written; and a raw UDP socket hears the datagrams that come for the machine, header and all
  CHECK(t.s(54, whole, 0, 3, t.word(0), 4) == 0 && send_to(whole, datagram, t.in4(10, 0, 0, 2, 0), 16) == 13 && t.lan.run_until([&] { return peer->pending() > 0; }, 3000));
  CHECK(peer->receive_from_ip(ByteView(seen), &seen_from, &seen_port, &seen_length) && seen_length == 5);
  CHECK(peer->send_to_ip(network::IpAddress(network::Ipv4Address::make(10, 0, 0, 1)), 4000, ConstByteView(reinterpret_cast<const uint8_t*>("back"), 4)));
  packet = receive(whole);
  CHECK(packet.size() == 20 + 8 + 4 && packet[9] == 17 && raw::be16(packet, 22) == 4000 && packet.substr(28) == "back");
  // IPPROTO_RAW: only sends, always whole packets
  const int64_t sender = t.s(41, kAfInet, kRaw | kNonBlock, 255);
  std::string echo_header("\x45\0\0\0\0\0\0\0\x05\x01\0\0\0\0\0\0\x0A\0\0\x02", 20);
  CHECK(sender >= 3 && t.option(sender, 0, 3, &v) == 0 && v == 1 && send_to(sender, raw::icmp(8, 1, 1, "x"), t.in4(10, 0, 0, 2, 0), 16) == -22);
  while (watcher->receive_from_ip(ByteView(seen), &seen_from, &seen_port, &seen_length)) {}
  CHECK(send_to(sender, echo_header + raw::icmp(8, 0x6161, 1, "by hand"), t.in4(10, 0, 0, 2, 0), 16) == 35 && t.lan.run_until([&] { return watcher->pending() > 0; }, 3000));
  CHECK(watcher->receive_from_ip(ByteView(seen), &seen_from, &seen_port, &seen_length) && seen_length == 35 && seen[8] == 5 && seen[20] == 8 && quiet(sender));
  packet = receive(icmp);      // (the answer comes to the raw ICMP socket)
  CHECK(packet.size() == 35 && raw::be16(packet, 24) == 0x6161);
  CHECK(t.s(3, sender) == 0 && t.s(3, whole) == 0);

  // IPv6: an echo socket, and a raw ICMPv6 socket (no IP header in what it gets; the checksum is the stack's to write) with ICMP6_FILTER
  const int64_t ping6 = t.s(41, kAfInet6, kDgram | kNonBlock, 58), icmp6 = t.s(41, kAfInet6, kRaw | kNonBlock, 58);
  CHECK(ping6 >= 3 && icmp6 >= 3 && t.option(icmp6, 1, 38, &v) == 0 && v == 58);
  // (the raw one hears neighbour discovery too: only echo replies are wanted here)
  const uint64_t filter = t.alloc(32);
  for (int i = 0; i < 8; ++i) t.put32(filter + 4 * uint64_t(i), i == 4 ? ~(1u << 1) : 0xFFFFFFFFu);      // everything but type 129
  CHECK(t.s(54, icmp6, 58, 1, filter, 32) == 0 && t.s(54, icmp6, 58, 1, filter, 16) == -22 && t.s(54, icmp6, 255, 1, filter, 4) == -92);
  t.put32(size, 32);
  CHECK(t.s(55, icmp6, 58, 1, buffer, size) == 0 && t.u32(size) == 32 && t.u32(buffer + 16) == ~(1u << 1));
  while (t.s(45, icmp6, buffer, 2048, 0, 0, 0) > 0) {}
  CHECK(send_to(ping6, raw::icmp(128, 0, 1, "six", false), t.in6(b6.global, 0), 28) == 11);
  reply = receive(ping6, &from, 28);
  CHECK(reply.size() == 11 && uint8_t(reply[0]) == 129 && raw::be16(reply, 6) == 1 && reply.substr(8) == "six" && from.size() == 28 && memcmp(from.data() + 8, b6.global.bytes, 16) == 0);
  reply = receive(icmp6);      // (the raw socket heard that reply as well)
  CHECK(reply.size() == 11 && uint8_t(reply[0]) == 129);
  CHECK(send_to(icmp6, raw::icmp(128, 0x7171, 2, "raw six", false), t.in6(b6.global, 0), 28) == 15);
  reply = receive(icmp6, &from, 28);
  CHECK(reply.size() == 15 && uint8_t(reply[0]) == 129 && raw::be16(reply, 4) == 0x7171 && reply.substr(8) == "raw six" && quiet(icmp6) && quiet(ping6));
  CHECK(t.s(54, icmp6, 41, 16, t.word(9), 4) == 0 && t.option(icmp6, 41, 16, &v) == 0 && v == 9);
  (void)a6;
  // closed sockets hear nothing more and their identifiers are free again
  CHECK(t.s(3, ping) == 0 && t.s(3, icmp) == 0 && t.s(3, ping6) == 0 && t.s(3, icmp6) == 0);
  const int64_t again = t.s(41, kAfInet, kDgram, 1);
  CHECK(t.s(49, again, t.in4(0, 0, 0, 0, identifier), 16) == 0 && t.s(3, again) == 0);
  for (int i = 0; i < 300; ++i) {
    const int64_t one = t.s(41, kAfInet, kRaw, 1);
    CHECK(one >= 3 && t.s(3, one) == 0);
  }
}

// --- socket filters and AF_PACKET ---------------------------------------------------------------------------------------------------------------------------
namespace filter {
// one instruction of a classic BPF program
std::string op(uint16_t code, uint8_t jt, uint8_t jf, uint32_t k) {
  std::string out;
  out.push_back(char(code)); out.push_back(char(code >> 8)); out.push_back(char(jt)); out.push_back(char(jf));
  for (int i = 0; i < 4; ++i) out.push_back(char(k >> (8 * i)));
  return out;
}
ConstByteView view(const std::string& bytes) { return ConstByteView(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()); }
// "arp": the ether type at 12 is 0x0806
std::string arp() { return op(0x28, 0, 0, 12) + op(0x15, 0, 1, 0x0806) + op(0x06, 0, 0, 0x40000) + op(0x06, 0, 0, 0); }
// "udp dst port N" over Ethernet and IPv4, as tcpdump writes it (the IP header's length from its first byte)
std::string udp_to(uint16_t port) {
  return op(0x28, 0, 0, 12) + op(0x15, 0, 8, 0x0800) + op(0x30, 0, 0, 23) + op(0x15, 0, 6, 17) + op(0x28, 0, 0, 20) + op(0x45, 4, 0, 0x1FFF) + op(0xB1, 0, 0, 14) + op(0x48, 0, 0, 16) +
         op(0x15, 0, 1, port) + op(0x06, 0, 0, 0x40000) + op(0x06, 0, 0, 0);
}
}  // namespace filter

static std::string ethernet(const uint8_t to[6], const uint8_t from[6], uint16_t type, const std::string& payload) {
  std::string f(reinterpret_cast<const char*>(to), 6);
  f += std::string(reinterpret_cast<const char*>(from), 6);
  f.push_back(char(type >> 8)); f.push_back(char(type));
  return f + payload;
}
// an ARP message: who has `target`? (operation 1) asked by `sender_mac` at `sender`
static std::string arp_message(uint16_t operation, const uint8_t sender_mac[6], const std::string& sender, const std::string& target) {
  std::string m("\0\x01\x08\0\x06\x04", 6);
  m.push_back(char(operation >> 8)); m.push_back(char(operation));
  m += std::string(reinterpret_cast<const char*>(sender_mac), 6) + sender + std::string(6, '\0') + target;
  return m;
}

static void test_socket_filter() {
  using namespace linuxabi;
  const uint8_t a[6] = {2, 0, 0, 0, 0, 1}, b[6] = {2, 0, 0, 0, 0, 2};
  const std::string arp_frame = ethernet(b, a, 0x0806, arp_message(1, a, std::string("\x0A\0\0\x01", 4), std::string("\x0A\0\0\x02", 4)));
  // an IPv4 UDP datagram to port 9999, with an IP header of 24 bytes (one option word)
  std::string ip("\x46\0\0\x24\0\0\0\0\x40\x11\0\0\x0A\0\0\x01\x0A\0\0\x02\0\0\0\0", 24);
  const std::string udp_frame = ethernet(b, a, 0x0800, ip + std::string("\x30\x39\x27\x0F\0\x0C\0\0data", 12));
  auto packet = [](const std::string& frame) {
    bpf::Packet p;
    p.data = filter::view(frame);
    p.network_offset = 14;
    p.wire_length = uint32_t(frame.size());
    p.protocol = uint16_t(uint8_t(frame[12]) << 8 | uint8_t(frame[13]));
    p.packet_type = 1;
    p.interface_index = 2;
    return p;
  };
  auto run = [&](const std::string& program, const std::string& frame) { return bpf::run(filter::view(program), packet(frame)); };
  CHECK(bpf::valid(filter::view(filter::arp())) && run(filter::arp(), arp_frame) == 0x40000 && run(filter::arp(), udp_frame) == 0);
  CHECK(bpf::valid(filter::view(filter::udp_to(9999))) && run(filter::udp_to(9999), udp_frame) == 0x40000 && run(filter::udp_to(9998), udp_frame) == 0 && run(filter::udp_to(9999), arp_frame) == 0);
  // every kind of instruction: constants, memory, arithmetic, the registers, the packet's length, facts beside the packet
  using filter::op;
  CHECK(run(op(0x00, 0, 0, 7) + op(0x02, 0, 0, 3) + op(0x00, 0, 0, 0) + op(0x60, 0, 0, 3) + op(0x16, 0, 0, 0), arp_frame) == 7);                                              // ld #7; st M[3]; ld #0; ld M[3]; ret a
  CHECK(run(op(0x00, 0, 0, 10) + op(0x04, 0, 0, 5) + op(0x24, 0, 0, 3) + op(0x14, 0, 0, 1) + op(0x34, 0, 0, 4) + op(0x94, 0, 0, 7) + op(0x16, 0, 0, 0), arp_frame) == ((10 + 5) * 3 - 1) / 4 % 7);
  CHECK(run(op(0x00, 0, 0, 0xF0) + op(0x54, 0, 0, 0x3C) + op(0x44, 0, 0, 1) + op(0xA4, 0, 0, 0xFF) + op(0x64, 0, 0, 4) + op(0x74, 0, 0, 2) + op(0x16, 0, 0, 0), arp_frame) == ((((0xF0 & 0x3C) | 1) ^ 0xFF) << 4) >> 2);
  CHECK(run(op(0x01, 0, 0, 9) + op(0x87, 0, 0, 0) + op(0x84, 0, 0, 0) + op(0x16, 0, 0, 0), arp_frame) == uint32_t(-9));                                                        // ldx #9; txa; neg; ret a
  CHECK(run(op(0x00, 0, 0, 6) + op(0x07, 0, 0, 0) + op(0x00, 0, 0, 40) + op(0x0C, 0, 0, 0) + op(0x1C, 0, 0, 0) + op(0x2C, 0, 0, 0) + op(0x3C, 0, 0, 0) + op(0x16, 0, 0, 0), arp_frame) == 40);      // + - * / by x
  CHECK(run(op(0x80, 0, 0, 0) + op(0x16, 0, 0, 0), arp_frame) == arp_frame.size() && run(op(0x81, 0, 0, 0) + op(0x87, 0, 0, 0) + op(0x16, 0, 0, 0), arp_frame) == arp_frame.size());
  CHECK(run(op(0x20, 0, 0, 0xFFFFF000u) + op(0x16, 0, 0, 0), arp_frame) == 0x0806 && run(op(0x20, 0, 0, 0xFFFFF004u) + op(0x16, 0, 0, 0), arp_frame) == 1 &&
        run(op(0x20, 0, 0, 0xFFFFF008u) + op(0x16, 0, 0, 0), arp_frame) == 2);      // the protocol, how it came by, the interface
  CHECK(run(op(0x30, 0, 0, 0xFFF00000u + 9) + op(0x16, 0, 0, 0), udp_frame) == 17 && run(op(0x28, 0, 0, 0xFFE00000u + 12) + op(0x16, 0, 0, 0), udp_frame) == 0x0800);      // from the network header, from the link header
  CHECK(run(op(0x01, 0, 0, 12) + op(0x48, 0, 0, 0) + op(0x16, 0, 0, 0), arp_frame) == 0x0806 && run(op(0x01, 0, 0, 2) + op(0x40, 0, 0, 10) + op(0x16, 0, 0, 0), arp_frame) == 0x08060001);      // indexed loads
  CHECK(run(op(0x05, 0, 0, 1) + op(0x06, 0, 0, 1) + op(0x06, 0, 0, 2), arp_frame) == 2);                                                                                      // ja
  CHECK(run(op(0x00, 0, 0, 5) + op(0x25, 0, 1, 4) + op(0x06, 0, 0, 1) + op(0x06, 0, 0, 2), arp_frame) == 1 && run(op(0x00, 0, 0, 5) + op(0x35, 0, 1, 6) + op(0x06, 0, 0, 1) + op(0x06, 0, 0, 2), arp_frame) == 2);
  CHECK(run(op(0x00, 0, 0, 6) + op(0x01, 0, 0, 2) + op(0x4D, 0, 1, 0) + op(0x06, 0, 0, 1) + op(0x06, 0, 0, 2), arp_frame) == 1);                                              // jset x
  // a load outside the packet, and a division by a register that is zero, end the program with nothing
  CHECK(run(op(0x20, 0, 0, uint32_t(arp_frame.size()) - 3) + op(0x06, 0, 0, 9), arp_frame) == 0 && run(op(0x20, 0, 0, uint32_t(arp_frame.size()) - 4) + op(0x06, 0, 0, 9), arp_frame) == 9);
  CHECK(run(op(0x01, 0, 0, 5000) + op(0x50, 0, 0, 0x20) + op(0x06, 0, 0, 9), arp_frame) == 0 && run(op(0x00, 0, 0, 8) + op(0x3C, 0, 0, 0) + op(0x06, 0, 0, 9), arp_frame) == 0 &&
        run(op(0x00, 0, 0, 8) + op(0x9C, 0, 0, 0) + op(0x06, 0, 0, 9), arp_frame) == 0 && run(op(0x00, 0, 0, 8) + op(0x01, 0, 0, 40) + op(0x6C, 0, 0, 0) + op(0x16, 0, 0, 0), arp_frame) == 0);
  // what is not a program
  auto bad = [](const std::string& program) { return !bpf::valid(filter::view(program)) && bpf::run(filter::view(program), bpf::Packet()) == 0; };
  CHECK(bad("") && bad(std::string(7, '\0')) && bad(op(0x00, 0, 0, 1)) && bad(op(0x06, 0, 0, 1) + op(0x00, 0, 0, 1)));                    // empty, not whole, no return at the end
  CHECK(bad(op(0x15, 1, 0, 0) + op(0x06, 0, 0, 1)) && bad(op(0x15, 0, 1, 0) + op(0x06, 0, 0, 1)) && bad(op(0x05, 0, 0, 1) + op(0x06, 0, 0, 1)) &&
        bad(op(0x05, 0, 0, 0xFFFFFFFFu) + op(0x06, 0, 0, 1) + op(0x06, 0, 0, 1)));                                                       // jumps past the end
  CHECK(bad(op(0x60, 0, 0, 16) + op(0x06, 0, 0, 1)) && bad(op(0x02, 0, 0, 16) + op(0x06, 0, 0, 1)) && bad(op(0x61, 0, 0, 99) + op(0x06, 0, 0, 1)));      // memory there is not
  CHECK(bad(op(0x34, 0, 0, 0) + op(0x06, 0, 0, 1)) && bad(op(0x94, 0, 0, 0) + op(0x06, 0, 0, 1)) && bad(op(0x64, 0, 0, 32) + op(0x06, 0, 0, 1)));          // by zero, by too much
  CHECK(bad(op(0x38, 0, 0, 0) + op(0x06, 0, 0, 1)) && bad(op(0xB4, 0, 0, 0) + op(0x06, 0, 0, 1)) && bad(op(0x55, 0, 0, 0) + op(0x06, 0, 0, 1)) && bad(op(0x106, 0, 0, 0)) &&
        bad(op(0x17, 0, 0, 0) + op(0x06, 0, 0, 1)));                                                // operations there are not; too long
  std::string longest;
  for (int i = 0; i < 4096; ++i) longest += op(0x06, 0, 0, 1);
  CHECK(bpf::valid(filter::view(longest)) && bad(longest + op(0x06, 0, 0, 1)));                                                           // 4096 instructions, and one more
  // Any bit of a program flipped, and programs of random bytes: refused, or run to an end on every packet (shorter ones too)
  const std::string programs[] = {filter::arp(), filter::udp_to(9999),
                                  op(0x00, 0, 0, 6) + op(0x07, 0, 0, 0) + op(0x00, 0, 0, 40) + op(0x0C, 0, 0, 0) + op(0x2C, 0, 0, 0) + op(0x3C, 0, 0, 0) + op(0x02, 0, 0, 3) + op(0x61, 0, 0, 3) +
                                      op(0x48, 0, 0, 2) + op(0x16, 0, 0, 0)};
  size_t accepted = 0, refused = 0;
  auto exercise = [&](const std::string& program) {
    if (!bpf::valid(filter::view(program))) { ++refused; return; }
    ++accepted;
    for (const std::string& frame : {arp_frame, udp_frame, udp_frame.substr(0, 20), std::string()}) (void)run(program, frame);
  };
  for (const std::string& program : programs)
    for (size_t bit = 0; bit < program.size() * 8; ++bit) {
      std::string flipped = program;
      flipped[bit / 8] = char(flipped[bit / 8] ^ (1 << (bit % 8)));
      exercise(flipped);
      exercise(flipped.substr(0, bit / 8));
    }
  uint32_t seed = 12345;
  for (int i = 0; i < 20000; ++i) {
    std::string random;
    const size_t count = 1 + (seed >> 8) % 12;
    for (size_t n = 0; n < count * 8; ++n) { seed = seed * 1664525u + 1013904223u; random.push_back(char((seed >> 16) % (n % 8 == 0 ? 0xC0 : n % 8 == 1 ? 2 : 256))); }
    random[random.size() - 8] = 0x06;      // (so that some get through the check)
    random[random.size() - 7] = 0;
    exercise(random);
  }
  CHECK(accepted > 500 && refused > 500);
}

static void test_packet_sockets() {
  constexpr uint64_t kPacket = 17, kRaw = 3;
  constexpr uint64_t kAllProtocols = 0x0300, kArp = 0x0608, kIp = 0x0008;      // htons(ETH_P_ALL), htons(ETH_P_ARP), htons(ETH_P_IP)
  Setup t;
  const uint8_t* mac_a = t.lan.nic_a.mac.bytes;
  const uint8_t* mac_b = t.lan.nic_b.mac.bytes;
  const uint8_t everyone[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  const std::string here("\x0A\0\0\x01", 4), there("\x0A\0\0\x02", 4);
  const uint64_t buffer = t.alloc(2048), name = t.alloc(32), size = t.alloc(8);
  uint32_t v = 0;
  // struct sockaddr_ll
  auto address = [&](uint16_t protocol, uint32_t interface, const uint8_t* to = nullptr) {
    const uint64_t at = t.alloc(24);
    uint8_t raw[20] = {17, 0, uint8_t(protocol >> 8), uint8_t(protocol), uint8_t(interface), 0, 0, 0, 0, 0, 0, uint8_t(to ? 6 : 0)};
    if (to) memcpy(raw + 12, to, 6);
    t.memory.write(at, ConstByteView(raw, 20));
    return at;
  };
  struct Got { std::string data, from; int64_t result = 0; };
  auto receive = [&](int64_t fd, uint64_t flags = 0) {
    Got g;
    if (!t.lan.run_until([&] { return t.polled(fd, kPollIn) == kPollIn; }, 3000)) { g.result = -11; return g; }
    t.put32(size, 20);
    g.result = t.s(45, fd, buffer, 2048, flags, name, size);
    if (g.result > 0) g.data = t.text(buffer, uint64_t(g.result));
    g.from = t.text(name, t.u32(size) <= 20 ? t.u32(size) : 0);
    return g;
  };
  auto quiet = [&](int64_t fd) { t.lan.step(30); return t.polled(fd, kPollIn) == 0; };
  auto send = [&](int64_t fd, const std::string& bytes, uint64_t to = 0) { return t.s(44, fd, t.bytes(bytes), bytes.size(), 0, to, to ? 20 : 0); };
  SharedPointer<network::IFrameTap> other = t.lan.b.frame_open();      // the other machine, tapping its own interface
  uint8_t seen[1514];
  network::FrameInfo info;
  CHECK(other);
  other->listen(0x0806, false, true, false, false);

  // what there is and is not
  CHECK(t.s(41, kPacket, kStream, kAllProtocols) == -94 && t.s(41, kPacket, kRaw, 0x10000) == -22);
  const int64_t arp = t.s(41, kPacket, kRaw | kNonBlock, kArp);
  CHECK(arp >= 3 && t.option(arp, 1, 3, &v) == 0 && v == kRaw && t.option(arp, 1, 39, &v) == 0 && v == kPacket && t.option(arp, 1, 38, &v) == 0 && v == kArp && t.polled(arp) == kPollOut);
  // bind: an interface that is there; getsockname shows it with its hardware address (18 bytes: up to the address's end)
  CHECK(t.s(49, arp, address(0, 9), 20) == -19 && t.s(49, arp, address(0, 2), 16) == -22 && t.s(49, arp, address(0, 2), 20) == 0);
  t.put32(size, 20);
  CHECK(t.s(51, arp, name, size) == 0 && t.u32(size) == 18 && t.text(name, 12) == std::string("\x11\0\x08\x06\x02\0\0\0\x01\0\0\x06", 12) && memcmp(t.text(name + 12, 6).data(), mac_a, 6) == 0);
  // "arping": a request written by hand goes out, the other machine answers, and the answer comes to the socket (and nothing else does)
  const std::string request = ethernet(everyone, mac_a, 0x0806, arp_message(1, mac_a, here, there));
  CHECK(send(arp, request) == int64_t(request.size()));
  Got g = receive(arp);
  CHECK(g.result >= 42 && g.data.substr(0, 14) == ethernet(mac_a, mac_b, 0x0806, "") && g.data.substr(14 + 6, 2) == std::string("\0\x02", 2) && g.data.substr(14 + 14, 4) == there);
  CHECK(g.from.size() == 20 && g.from.substr(0, 12) == std::string("\x11\0\x08\x06\x02\0\0\0\x01\0\0\x06", 12) && memcmp(g.from.data() + 12, mac_b, 6) == 0 && quiet(arp));      // ARPHRD_ETHER, PACKET_HOST
  CHECK(other->receive(ByteView(seen), &info) && info.kind == network::FrameKind::Broadcast && info.length >= request.size() && memcmp(seen, request.data(), request.size()) == 0);
  // a broadcast from the other machine: looked at first, sized, then read; FIONREAD
  const std::string asked = ethernet(everyone, mac_b, 0x0806, arp_message(1, mac_b, there, std::string("\x0A\0\0\x63", 4)));
  CHECK(other->send(ConstByteView(reinterpret_cast<const uint8_t*>(asked.data()), asked.size())) && t.lan.run_until([&] { return t.polled(arp, kPollIn) == kPollIn; }, 3000));
  CHECK(t.s(16, arp, 0x541B, size) == 0 && t.u32(size) >= asked.size() && t.s(45, arp, buffer, 10, 0x22 /* MSG_PEEK | MSG_TRUNC */, 0, 0) == int64_t(t.u32(size)));      // (the wire pads a short frame)
  g = receive(arp);
  CHECK(g.data.substr(0, asked.size()) == asked && g.from[10] == 1 /* PACKET_BROADCAST */ && t.s(45, arp, buffer, 64, 0, 0, 0) == -11);

  // SOCK_DGRAM: no link header either way; the address says where a frame goes and where one came from
  const int64_t cooked = t.s(41, kPacket, kDgram | kNonBlock, kArp);
  const std::string question = arp_message(1, mac_a, here, there);
  CHECK(cooked >= 3 && t.option(cooked, 1, 3, &v) == 0 && v == kDgram && send(cooked, question) == -6 && send(cooked, question, address(0, 2)) == -22);      // no interface; nowhere to send it
  CHECK(send(cooked, question, address(0x0806, 2, everyone)) == int64_t(question.size()));
  g = receive(cooked);
  CHECK(g.result >= 28 && g.data.substr(6, 2) == std::string("\0\x02", 2) && g.data.substr(8, 6) == std::string(reinterpret_cast<const char*>(mac_b), 6) && memcmp(g.from.data() + 12, mac_b, 6) == 0);
  g = receive(arp);      // (the first socket hears the same answer, whole)
  CHECK(g.result >= 42 && g.data.substr(12, 2) == std::string("\x08\x06", 2));

  // ETH_P_ALL: what the interface sends too (PACKET_OUTGOING), unless the socket says it does not want that
  const int64_t all = t.s(41, kPacket, kRaw | kNonBlock, kAllProtocols), udp = t.s(41, kAfInet, kDgram, 0);
  SharedPointer<network::IUdpSocket> peer = t.lan.b.udp_open_families(9999, network::IpFamilies::Both);
  CHECK(all >= 3 && peer && t.s(49, all, address(0, 2), 20) == 0 && t.s(44, udp, t.bytes("out"), 3, 0, t.in4(10, 0, 0, 2, 9999), 16) == 3);
  g = receive(all);
  CHECK(g.result == 14 + 20 + 8 + 3 && g.data.substr(0, 14) == ethernet(mac_b, mac_a, 0x0800, "") && g.data.substr(g.data.size() - 3) == "out" && g.from[10] == 4 && g.from.substr(2, 2) == std::string("\x08\0", 2));
  network::IpAddress from;
  uint16_t from_port = 0;
  uint32_t length = 0;
  CHECK(t.lan.run_until([&] { return peer->pending() > 0; }, 3000) && peer->receive_from_ip(ByteView(seen), &from, &from_port, &length) && length == 3);
  CHECK(peer->send_to_ip(from, from_port, ConstByteView(reinterpret_cast<const uint8_t*>("back"), 4)));
  g = receive(all);
  CHECK(g.result >= 14 + 20 + 8 + 4 && g.from[10] == 0 && g.data.substr(14 + 20 + 8, 4) == "back" && quiet(arp));      // (incoming: PACKET_HOST; and the ARP socket hears no IP)
  CHECK(t.s(54, all, 263, 23, t.word(1), 4) == 0 && t.option(all, 263, 23, &v) == 0 && v == 1 && t.s(44, udp, t.bytes("unseen"), 6, 0, t.in4(10, 0, 0, 2, 9999), 16) == 6 && quiet(all));
  CHECK(t.s(54, all, 263, 23, t.word(0), 4) == 0);

  // A filter (SO_ATTACH_FILTER) picks what the socket gets: here UDP to port 9999, of which 20 bytes
  const std::string program = filter::udp_to(9999).substr(0, 9 * 8) + filter::op(0x06, 0, 0, 20) + filter::op(0x06, 0, 0, 0);
  auto attach = [&](int64_t fd, const std::string& code, uint64_t count) {
    const uint64_t at = t.alloc(16);
    t.put64(at, count); t.put64(at + 8, t.bytes(code));
    return t.s(54, fd, 1, 26, at, 16);
  };
  CHECK(attach(all, program, program.size() / 8) == 0 && attach(all, program.substr(0, 16), 2) == -22 && attach(all, program, 0) == -22 && attach(all, program, 5000) == -22);
  while (peer->receive_from_ip(ByteView(seen), &from, &from_port, &length)) {}
  CHECK(t.s(44, udp, t.bytes("other port"), 10, 0, t.in4(10, 0, 0, 2, 9998), 16) == 10 && quiet(all) && t.s(44, udp, t.bytes("this port"), 9, 0, t.in4(10, 0, 0, 2, 9999), 16) == 9);
  // recvmsg: MSG_TRUNC in what comes back when the filter kept less than the frame, and PACKET_AUXDATA says how much there was
  {
    const uint64_t vectors = t.alloc(16), message = t.alloc(56), control = t.alloc(64);
    CHECK(t.s(54, all, 263, 8, t.word(1), 4) == 0 && t.lan.run_until([&] { return t.polled(all, kPollIn) == kPollIn; }, 3000));
    t.put64(vectors, buffer); t.put64(vectors + 8, 2048);
    t.put64(message, name); t.put32(message + 8, 20); t.put64(message + 16, vectors); t.put64(message + 24, 1); t.put64(message + 32, control); t.put64(message + 40, 64);
    CHECK(t.s(47, all, message, 0) == 20 && t.u32(message + 48) == 0x20 && t.u32(message + 8) == 20 && t.u64(message + 40) == 40 && t.u32(control + 8) == 263 && t.u32(control + 12) == 8 &&
          t.u32(control + 16) == 1 && t.u32(control + 20) == 14 + 20 + 8 + 9 && t.u32(control + 24) == 20 && t.text(buffer, 14) == ethernet(mac_b, mac_a, 0x0800, ""));
  }
  CHECK(t.s(54, all, 1, 27, 0, 0) == -22 && t.s(54, all, 1, 27, t.word(0), 4) == 0 && t.s(54, all, 1, 27, t.word(0), 4) == -2 && attach(all, filter::arp(), 4) == 0 && t.s(54, all, 1, 44, t.word(1), 4) == 0 &&
        t.s(54, all, 1, 27, t.word(0), 4) == -1 &&
        attach(all, program, program.size() / 8) == -1);      // (taken off; none to take off; a locked one stays)
  // PACKET_STATISTICS counts what the socket was given since it was last asked
  t.put32(size, 8);
  CHECK(t.s(55, all, 263, 6, buffer, size) == 0 && t.u32(size) == 8 && t.u32(buffer) == 3 && t.u32(buffer + 4) == 0 && t.s(55, all, 263, 6, buffer, size) == 0 && t.u32(buffer) == 0);

  // the loopback: packets the machine sends to itself, shown as frames with no addresses, once each
  const int64_t loop = t.s(41, kPacket, kRaw | kNonBlock, kIp), local = t.s(41, kAfInet, kDgram, 0);
  CHECK(t.s(49, loop, address(0, 1), 20) == 0 && t.s(49, local, t.in4(127, 0, 0, 1, 9997), 16) == 0 && t.s(44, udp, t.bytes("to myself"), 9, 0, t.in4(127, 0, 0, 1, 9997), 16) == 9);
  g = receive(loop);
  CHECK(g.result == 14 + 20 + 8 + 9 && g.data.substr(0, 14) == std::string(12, '\0') + std::string("\x08\0", 2) && g.data.substr(g.data.size() - 9) == "to myself" &&
        g.from.substr(0, 12) == std::string("\x11\0\x08\0\x01\0\0\0\x04\x03\0\x06", 12) && quiet(loop));      // interface 1, ARPHRD_LOOPBACK, PACKET_HOST
  t.put32(size, 20);
  CHECK(t.s(51, loop, name, size) == 0 && t.u32(size) == 18 && t.text(name, 12) == std::string("\x11\0\x08\0\x01\0\0\0\x04\x03\0\x06", 12));
  CHECK(t.s(45, local, buffer, 64, 0, 0, 0) == 9);
  // a frame sent on the loopback comes back to the machine: here the datagram just heard, sent again
  CHECK(send(loop, g.data) == int64_t(g.data.size()) && t.lan.run_until([&] { return t.polled(local, kPollIn) == kPollIn; }, 3000) && t.s(45, local, buffer, 64, 0, 0, 0) == 9 &&
        t.text(buffer, 9) == "to myself");
  g = receive(loop);
  CHECK(g.result == 14 + 20 + 8 + 9 && quiet(arp));
  // SO_BINDTODEVICE names the interface; an unbound socket hears every interface
  CHECK(t.s(54, loop, 1, 25, t.bytes(std::string("eth0\0", 5)), 5) == 0 && t.s(54, loop, 1, 25, t.bytes(std::string("nope\0", 5)), 5) == -19);
  t.put32(size, 20);
  CHECK(t.s(51, loop, name, size) == 0 && t.u32(name + 4) == 2 && t.s(54, loop, 1, 25, t.bytes(std::string(1, '\0')), 1) == 0 && t.s(51, loop, name, size) == 0 && t.u32(name + 4) == 0 && t.u32(size) == 12);
  CHECK(t.s(44, udp, t.bytes("one"), 3, 0, t.in4(127, 0, 0, 1, 9997), 16) == 3 && peer->send_to_ip(from, from_port, ConstByteView(reinterpret_cast<const uint8_t*>("two"), 3)));
  Got first = receive(loop), second = receive(loop);
  CHECK(first.result > 0 && second.result > 0 && first.from[4] + second.from[4] == 3 && quiet(loop));      // (one from each interface)

  // what a packet socket is not, and what it refuses
  CHECK(t.s(50, arp, 1) == -95 && t.s(43, arp, 0, 0) == -95 && t.s(42, arp, address(0, 2), 20) == -95 && t.s(48, arp, 2) == -95 && t.s(52, arp, name, size) == -95);
  CHECK(send(arp, std::string(13, 'x')) == -22 && send(arp, std::string(1515, 'x')) == -90 && send(arp, request, address(0, 7)) == -6 && t.s(44, arp, kBad, 60, 0, 0, 0) == -14);
  CHECK(t.s(54, arp, 263, 18, t.word(1), 4) == -92 && t.s(54, arp, 6, 1, t.word(1), 4) == -92);      // PACKET_FANOUT; another level
  CHECK(t.s(54, arp, 263, 1, buffer, 16) == 0 && t.s(54, arp, 263, 1, buffer, 8) == -22);      // PACKET_ADD_MEMBERSHIP (promiscuous mode): taken
  // read and write, readv and writev; the interface ioctls
  {
    const uint64_t vectors = t.alloc(32);
    CHECK(t.s(1, arp, t.bytes(request), request.size()) == int64_t(request.size()) && t.lan.run_until([&] { return t.polled(arp, kPollIn) == kPollIn; }, 3000) && t.s(0, arp, buffer, 2048) >= 42);
    t.put64(vectors, t.bytes(request.substr(0, 14))); t.put64(vectors + 8, 14); t.put64(vectors + 16, t.bytes(request.substr(14))); t.put64(vectors + 24, request.size() - 14);
    CHECK(t.s(20, arp, vectors, 2) == int64_t(request.size()) && t.lan.run_until([&] { return t.polled(arp, kPollIn) == kPollIn; }, 3000));
    t.put64(vectors, buffer); t.put64(vectors + 8, 14); t.put64(vectors + 16, buffer + 100); t.put64(vectors + 24, 2000);
    CHECK(t.s(19, arp, vectors, 2) >= 42 && t.text(buffer, 14) == ethernet(mac_a, mac_b, 0x0806, "") && t.text(buffer + 106, 2) == std::string("\0\x02", 2));
    const uint64_t ifreq = t.alloc(40);
    t.memory.write(ifreq, ConstByteView(reinterpret_cast<const uint8_t*>("eth0"), 5));
    CHECK(t.s(16, arp, 0x8933, ifreq) == 0 && t.u32(ifreq + 16) == 2 && t.s(16, arp, 0x8927, ifreq) == 0 && memcmp(t.text(ifreq + 18, 6).data(), mac_a, 6) == 0);
  }
  // closed sockets hear nothing more; many more than there can be at once
  CHECK(t.s(3, arp) == 0 && t.s(3, cooked) == 0 && t.s(3, all) == 0 && t.s(3, loop) == 0);
  for (int i = 0; i < 100; ++i) {
    const int64_t one = t.s(41, kPacket, kRaw, kAllProtocols);
    CHECK(one >= 3 && t.s(3, one) == 0);
  }
}

// --- packet rings -------------------------------------------------------------------------------------------------------------------------------------------
namespace rings {
uint32_t u32(ConstByteView ring, uint64_t at) { return uint32_t(ring[at]) | uint32_t(ring[at + 1]) << 8 | uint32_t(ring[at + 2]) << 16 | uint32_t(ring[at + 3]) << 24; }
uint16_t u16(ConstByteView ring, uint64_t at) { return uint16_t(ring[at] | ring[at + 1] << 8); }
void put32(ByteView ring, uint64_t at, uint32_t v) { for (int i = 0; i < 4; ++i) ring[at + uint64_t(i)] = uint8_t(v >> (8 * i)); }
std::string bytes(ConstByteView ring, uint64_t at, uint64_t n) { return std::string(reinterpret_cast<const char*>(ring.subview(at, n).data()), n); }
}  // namespace rings

// The ring layouts on their own: frames in, frames out, a ring that is full, and a program that writes nonsense into its side.
static void test_packet_ring_formats() {
  using namespace linuxabi::ring;
  using rings::u32; using rings::u16;
  CHECK(header_size(0) == 32 && header_size(1) == 32 && header_size(2) == 48 && header_size(3) == 0);
  Request r;
  r.block_size = 4096; r.block_count = 2; r.frame_size = 2048; r.frame_count = 4;
  CHECK(valid(Version::V1, r, 0) && valid(Version::V2, r, 0) && valid(Version::V3, r, 0));
  Request bad = r;
  bad.block_size = 4000; CHECK(!valid(Version::V2, bad, 0));          // not whole pages
  bad = r; bad.frame_count = 5; CHECK(!valid(Version::V2, bad, 0));   // not what the blocks hold
  bad = r; bad.frame_size = 2040; CHECK(!valid(Version::V2, bad, 0)); // not a multiple of 16
  bad = r; bad.frame_size = 48; bad.frame_count = 170; CHECK(!valid(Version::V2, bad, 0));      // less than a header and an address
  bad = r; bad.frame_size = 8192; bad.frame_count = 0; CHECK(!valid(Version::V2, bad, 0));      // larger than a block
  bad = r; bad.block_count = 0; bad.frame_count = 0; CHECK(!valid(Version::V2, bad, 0));
  bad = r; bad.private_size = 5000; CHECK(!valid(Version::V3, bad, 0) && valid(Version::V2, bad, 0));
  bad = r; bad.frame_size = 64; bad.frame_count = 128; CHECK(valid(Version::V2, bad, 0) && !valid(Version::V2, bad, 16) && !valid(Version::V3, bad, 0));

  const uint8_t link[14] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 0x08, 0x00};
  uint8_t name[20];
  for (int i = 0; i < 20; ++i) name[i] = uint8_t(0xA0 + i);
  std::string payload;
  for (int i = 0; i < 300; ++i) payload.push_back(char('a' + i % 26));
  auto frame = [&](bool with_link, uint32_t kept = 1 << 20) {
    Frame f;
    if (with_link) f.link = ConstByteView(link);
    f.data = ConstByteView(reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
    f.kept = kept;
    f.name = ConstByteView(name);
    f.time_ns = 1700000000123456789ull;
    return f;
  };
  std::vector<uint8_t> memory(8192);
  const ByteView ring(memory.data(), memory.size());
  // TPACKET_V2, whole frames: the header, the address at 32, the link header at 66 and what follows it at 80
  Receiver receiver;
  receiver.start(Version::V2, r, 0);
  CHECK(!receiver.readable(ring) && receiver.put(ring, frame(true), 0) && receiver.readable(ring));
  CHECK(u32(ring, 0) == 1 && u32(ring, 4) == 314 && u32(ring, 8) == 314 && u16(ring, 12) == 66 && u16(ring, 14) == 80 && u32(ring, 16) == 1700000000 && u32(ring, 20) == 123456789);
  CHECK(rings::bytes(ring, 32, 20) == std::string(reinterpret_cast<const char*>(name), 20) && rings::bytes(ring, 66, 14) == std::string(reinterpret_cast<const char*>(link), 14) &&
        rings::bytes(ring, 80, 300) == payload);
  // the next ones go into the next places; one cut short by the filter; a ring the program has not emptied takes no more
  CHECK(receiver.put(ring, frame(true, 100), 0) && u32(ring, 2048) == 1 && u32(ring, 2048 + 4) == 314 && u32(ring, 2048 + 8) == 100 && rings::bytes(ring, 2048 + 80, 86) == payload.substr(0, 86));
  CHECK(receiver.put(ring, frame(true), 0) && receiver.put(ring, frame(true), 0) && u32(ring, 4096) == 1 && u32(ring, 6144) == 1 && !receiver.put(ring, frame(true), 0));
  rings::put32(ring, 0, 0);      // (the program gives the first place back)
  CHECK(receiver.put(ring, frame(true), 0) && u32(ring, 0) == 1 && !receiver.put(ring, frame(true), 0));
  // a frame larger than a place is cut to it, and marked (TP_STATUS_COPY)
  {
    Request small = r;
    small.frame_size = 256; small.frame_count = 32;
    Receiver cut;
    cut.start(Version::V2, small, 0);
    std::fill(memory.begin(), memory.end(), 0);
    CHECK(cut.put(ring, frame(true), 0) && u32(ring, 0) == 3 && u32(ring, 4) == 314 && u32(ring, 8) == 256 - 66 && rings::bytes(ring, 80, 256 - 80) == payload.substr(0, 256 - 80));
  }
  // TPACKET_V1: the same places, a status of eight bytes, and microseconds; without link headers what follows them is at 80 too
  std::fill(memory.begin(), memory.end(), 0);
  receiver.start(Version::V1, r, 0);
  CHECK(receiver.put(ring, frame(true), 0) && u32(ring, 0) == 1 && u32(ring, 4) == 0 && u32(ring, 8) == 314 && u32(ring, 12) == 314 && u16(ring, 16) == 66 && u16(ring, 18) == 80 &&
        u32(ring, 20) == 1700000000 && u32(ring, 24) == 123456 && rings::bytes(ring, 80, 300) == payload);
  CHECK(receiver.put(ring, frame(false), 0) && u32(ring, 2048 + 8) == 300 && u16(ring, 2048 + 16) == 80 && u16(ring, 2048 + 18) == 80 && rings::bytes(ring, 2048 + 80, 300) == payload);
  // PACKET_RESERVE moves the frame further in
  std::fill(memory.begin(), memory.end(), 0);
  receiver.start(Version::V2, r, 32);
  CHECK(receiver.put(ring, frame(true), 0) && u16(ring, 12) == 98 && u16(ring, 14) == 112 && rings::bytes(ring, 112, 300) == payload);

  // TPACKET_V3: blocks. A block's header, then frames one after another; the block is the program's when it is full or has waited long enough
  std::fill(memory.begin(), memory.end(), 0);
  Request three = r;
  three.retire_ms = 50;
  receiver.start(Version::V3, three, 0);
  CHECK(receiver.put(ring, frame(true), 1000) && !receiver.readable(ring) && u32(ring, 0) == 2 && u32(ring, 4) == 48 && u32(ring, 8) == 0 && u32(ring, 12) == 1 && u32(ring, 16) == 48);
  const uint32_t each = (82 + 314 + 7) & ~7u;      // a frame's header and address, the frame at 82, to a multiple of 8
  CHECK(u32(ring, 48) == each && u32(ring, 48 + 4) == 1700000000 && u32(ring, 48 + 8) == 123456789 && u32(ring, 48 + 12) == 314 && u32(ring, 48 + 16) == 314 && u32(ring, 48 + 20) == 1 &&
        u16(ring, 48 + 24) == 82 && u16(ring, 48 + 26) == 96 && rings::bytes(ring, 48 + 48, 20) == std::string(reinterpret_cast<const char*>(name), 20) &&
        rings::bytes(ring, 48 + 82, 14) == std::string(reinterpret_cast<const char*>(link), 14) && rings::bytes(ring, 48 + 96, 300) == payload);
  receiver.tick(ring, 1000 + 49000000);
  CHECK(!receiver.readable(ring));
  receiver.tick(ring, 1000 + 50000000);      // (waited long enough: handed over, marked as timed out)
  CHECK(receiver.readable(ring) && u32(ring, 8) == 0x21 && u32(ring, 12) == 1 && u32(ring, 20) == 48 + each && u32(ring, 48) == 0 && u32(ring, 24) == 1 && u32(ring, 32) == 1700000000);
  // a block fills: the frame that does not fit closes it and opens the next; with every block the program's, frames are lost until it gives one back
  int put = 0;
  while (receiver.put(ring, frame(true), 2000) && put < 100) ++put;
  const uint32_t per_block = (4096 - 48) / each;
  CHECK(put == int(per_block) && u32(ring, 4096 + 8) == 1 && u32(ring, 4096 + 12) == per_block && u32(ring, 4096 + 24) == 2 && receiver.frozen() == 1 &&
        !receiver.put(ring, frame(true), 2000) && receiver.frozen() == 1);      // (the second block: full, handed over, the second in order; then nowhere to go)
  // walking a block the way a program does: by tp_next_offset, as many frames as num_pkts says
  {
    uint32_t at = u32(ring, 4096 + 16), seen = 0;
    for (uint32_t i = 0; i < u32(ring, 4096 + 12); ++i) {
      CHECK(u32(ring, 4096 + at + 12) == 314 && rings::bytes(ring, 4096 + at + 96, 300) == payload);
      ++seen;
      const uint32_t next = u32(ring, 4096 + at);
      CHECK((next == 0) == (i + 1 == u32(ring, 4096 + 12)));
      at += next;
    }
    CHECK(seen == per_block);
  }
  rings::put32(ring, 8, 0);
  CHECK(receiver.put(ring, frame(true), 3000) && u32(ring, 24) == 3 && u32(ring, 12) == 1);      // (the first block again, the third in order)

  // The sending side: what a program left to be sent, in order; a frame that makes no sense is marked and passed over
  std::fill(memory.begin(), memory.end(), 0);
  Transmitter transmitter;
  transmitter.start(Version::V2, r);
  using Wrong = Transmitter::Wrong;
  ConstByteView out;
  Wrong wrong = Wrong::No;
  CHECK(!transmitter.next(ring, false, 1514, false, &out, &wrong));
  auto leave = [&](uint64_t slot, uint32_t length, uint32_t status = 1) {
    rings::put32(ring, slot + 4, length);
    for (uint32_t i = 0; i < 64; ++i) ring[slot + 32 + i] = uint8_t(slot / 2048 * 16 + i);
    rings::put32(ring, slot, status);
  };
  leave(0, 60); leave(2048, 3000); leave(4096, 0); leave(6144, 42);
  CHECK(transmitter.next(ring, false, 1514, false, &out, &wrong) && wrong == Wrong::No && out.size() == 60 && out[0] == 0 && out[59] == 59 && u32(ring, 0) == 0);
  // one too long for its place stays at the head, marked, until the program puts it right; with PACKET_LOSS it is passed over
  CHECK(transmitter.next(ring, false, 1514, false, &out, &wrong) && wrong == Wrong::TooLong && u32(ring, 2048) == 4 && !transmitter.next(ring, false, 1514, false, &out, &wrong));
  leave(2048, 30);
  CHECK(transmitter.next(ring, false, 1514, false, &out, &wrong) && wrong == Wrong::No && out.size() == 30);
  CHECK(transmitter.next(ring, false, 1514, true, &out, &wrong) && wrong == Wrong::Malformed && u32(ring, 4096) == 0);      // (no length at all)
  CHECK(transmitter.next(ring, false, 1514, false, &out, &wrong) && wrong == Wrong::No && out.size() == 42 && out[0] == 48 && !transmitter.next(ring, false, 1514, false, &out, &wrong));
  leave(0, 1600);
  CHECK(transmitter.next(ring, false, 1514, true, &out, &wrong) && wrong == Wrong::TooLong);      // (more than a frame may be)
  // PACKET_TX_HAS_OFF: the header says where the frame starts
  leave(2048, 20);
  ring[2048 + 12] = 100; ring[2048 + 13] = 0;
  CHECK(transmitter.next(ring, true, 1514, false, &out, &wrong) && wrong == Wrong::No && out.size() == 20 && out.data() == ring.data() + 2048 + 100);
  leave(4096, 20);
  ring[4096 + 12] = 8; ring[4096 + 13] = 0;      // (inside the header)
  CHECK(transmitter.next(ring, true, 1514, false, &out, &wrong) && wrong == Wrong::Malformed);
  // Any nonsense in the ring: whatever a program writes there, both sides stay inside it (the sanitizers watch) and the walk ends
  uint32_t seed = 99;
  for (int round = 0; round < 3000; ++round) {
    for (uint8_t& b : memory) { seed = seed * 1664525u + 1013904223u; b = uint8_t((seed >> 24) & (round % 3 == 0 ? 0x01 : 0xFF)); }
    const Version version = Version(round % 3);
    Request any = r;
    if (round % 2) { any.frame_size = 128; any.frame_count = 64; }
    Transmitter t;
    t.start(version, any);
    for (int i = 0; i < 80; ++i) {
      ConstByteView taken;
      Wrong w = Wrong::No;
      if (!t.next(ring, round % 5 == 0, 1514, true, &taken, &w)) break;
      CHECK(w != Wrong::No || (taken.data() >= ring.data() && taken.data() + taken.size() <= ring.data() + ring.size() && taken.size() <= 1514));
    }
    Receiver into;
    into.start(version, any, uint32_t(round % 4) * 8);
    for (int i = 0; i < 40; ++i) (void)into.put(ring, frame(i % 2 == 0, uint32_t(seed >> 20)), uint64_t(i) * 30000000);
    into.tick(ring, 5000000000ull);
    (void)into.readable(ring);
  }
}

// Rings through the socket calls. (mmap() itself needs an address space, which this rig has none of: the test looks at the same memory through the
// call mmap() uses. The real mapping is exercised on the kernel, by tests/linux/ringtest.c.)
static void test_packet_ring_sockets() {
  constexpr uint64_t kPacket = 17, kRaw = 3, kAllProtocols = 0x0300, kArp = 0x0608;
  using rings::u32; using rings::u16;
  Setup t;
  const uint8_t* mac_a = t.lan.nic_a.mac.bytes;
  const uint8_t* mac_b = t.lan.nic_b.mac.bytes;
  const uint8_t everyone[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  const std::string here("\x0A\0\0\x01", 4), there("\x0A\0\0\x02", 4);
  uint32_t v = 0;
  auto request = [&](uint32_t block_size, uint32_t blocks, uint32_t frame_size, uint32_t frames, uint32_t retire_ms = 0) {
    const uint64_t at = t.alloc(32);
    t.put32(at, block_size); t.put32(at + 4, blocks); t.put32(at + 8, frame_size); t.put32(at + 12, frames); t.put32(at + 16, retire_ms); t.put32(at + 20, 0); t.put32(at + 24, 0);
    return at;
  };
  auto ring_of = [&](int64_t fd, int which, uint32_t* count = nullptr) {
    uint64_t link = 0;
    BlockRef blocks[2];
    uint64_t sizes[2] = {};
    const uint32_t n = linuxabi::packet_descriptor(t.process, uint64_t(fd), &link) ? linuxabi::packet_rings(link, blocks, sizes) : 0;
    if (count) *count = n;
    return uint32_t(which) < n ? blocks[which].get()->bytes().first(sizes[which]) : ByteView();      // (the socket keeps the memory alive)
  };
  const uint64_t address = t.alloc(24), size = t.alloc(8), buffer = t.alloc(2048);
  {
    const uint8_t raw[20] = {17, 0, 0, 0, 2};
    t.memory.write(address, ConstByteView(raw, 20));
  }
  SharedPointer<network::IFrameTap> other = t.lan.b.frame_open();
  CHECK(other);
  other->listen(0x0806, false, true, false, false);
  const std::string asked = ethernet(everyone, mac_b, 0x0806, arp_message(1, mac_b, there, std::string("\x0A\0\0\x63", 4)));
  auto arrives = [&](int64_t fd) {
    return other->send(ConstByteView(reinterpret_cast<const uint8_t*>(asked.data()), asked.size())) && t.lan.run_until([&] { return t.polled(fd, kPollIn) == kPollIn; }, 3000);
  };

  // the versions, and the size of a frame's header in each
  const int64_t fd = t.s(41, kPacket, kRaw | kNonBlock, kArp);
  CHECK(fd >= 3 && t.s(49, fd, address, 20) == 0 && t.option(fd, 263, 10, &v) == 0 && v == 0);
  for (uint32_t version = 0; version < 4; ++version) {
    t.put32(buffer, version); t.put32(size, 4);
    const int64_t r = t.s(55, fd, 263, 11, buffer, size);
    CHECK(version < 3 ? (r == 0 && t.u32(buffer) == (version == 2 ? 48u : 32u)) : r == -22);
  }
  CHECK(t.s(54, fd, 263, 10, t.word(3), 4) == -22 && t.s(54, fd, 263, 10, t.word(1), 4) == 0 && t.option(fd, 263, 10, &v) == 0 && v == 1);
  // a receive ring: what it may be; with one, the version and the reserve stay as they are
  CHECK(t.s(54, fd, 263, 5, request(4096, 2, 2048, 5), 16) == -22 && t.s(54, fd, 263, 5, request(4000, 2, 2000, 4), 16) == -22 && t.s(54, fd, 263, 5, request(4096, 2, 2048, 4), 8) == -22 &&
        t.s(54, fd, 263, 5, request(1 << 20, 100, 2048, 51200), 16) == -12 && t.s(54, fd, 263, 5, kBad, 16) == -14);
  CHECK(t.s(54, fd, 263, 5, request(4096, 2, 2048, 4), 16) == 0 && t.s(54, fd, 263, 10, t.word(0), 4) == -16 && t.s(54, fd, 263, 12, t.word(8), 4) == -16);
  uint32_t count = 0;
  ByteView ring = ring_of(fd, 0, &count);
  CHECK(count == 1 && ring.size() == 8192 && t.polled(fd) == kPollOut);
  // a frame arrives: poll says so, and it is in the ring (TPACKET_V2: status, lengths, where it is, its address)
  CHECK(arrives(fd) && u32(ring, 0) == 1 && u32(ring, 4) >= asked.size() && u32(ring, 8) == u32(ring, 4) && u16(ring, 12) == 66 && u16(ring, 14) == 80 &&
        rings::bytes(ring, 66, asked.size()) == asked && rings::bytes(ring, 32, 12) == std::string("\x11\0\x08\x06\x02\0\0\0\x01\0\x01\x06", 12) && memcmp(ring.data() + 44, mac_b, 6) == 0);
  CHECK(t.s(45, fd, buffer, 2048, 0, 0, 0) == -11);      // (nothing comes by recv() any more)
  // poll goes on saying so until the program gives the frame back; then the next one goes into the next place
  CHECK(t.polled(fd, kPollIn) == kPollIn);
  rings::put32(ring, 0, 0);
  CHECK(t.polled(fd, kPollIn) == 0 && arrives(fd) && u32(ring, 2048) == 1 && u32(ring, 0) == 0);
  // a ring that is full loses frames, and the statistics say so
  for (int i = 0; i < 5; ++i) CHECK(other->send(ConstByteView(reinterpret_cast<const uint8_t*>(asked.data()), asked.size())));
  t.lan.step(30);
  CHECK(t.polled(fd, kPollIn) == kPollIn && u32(ring, 0) == 1 && u32(ring, 4096) == 1 && u32(ring, 6144) == 1);
  t.put32(size, 8);
  CHECK(t.s(55, fd, 263, 6, buffer, size) == 0 && t.u32(size) == 8 && t.u32(buffer) == 7 && t.u32(buffer + 4) == 2);
  // a filter cuts what goes into the ring too
  {
    const std::string program = filter::op(0x06, 0, 0, 20);
    const uint64_t at = t.alloc(16);
    t.put64(at, 1); t.put64(at + 8, t.bytes(program));
    for (uint64_t slot = 0; slot < 8192; slot += 2048) rings::put32(ring, slot, 0);
    CHECK(t.s(54, fd, 1, 26, at, 16) == 0 && arrives(fd));
    uint64_t slot = 0;
    while (slot < 8192 && u32(ring, slot) == 0) slot += 2048;
    CHECK(slot < 8192 && u32(ring, slot + 4) >= asked.size() && u32(ring, slot + 8) == 20);
  }
  // a ring that is mapped stays: here the test holds it as a mapping would (the block's uses say so); without that it can be taken away again
  {
    uint64_t link = 0;
    BlockRef blocks[2];
    uint64_t sizes[2] = {};
    CHECK(linuxabi::packet_descriptor(t.process, uint64_t(fd), &link) && linuxabi::packet_rings(link, blocks, sizes) == 1);
    CHECK(t.s(54, fd, 263, 5, request(0, 0, 0, 0), 16) == -16 && t.s(54, fd, 263, 5, request(4096, 1, 2048, 2), 16) == -16);
  }
  CHECK(t.s(54, fd, 263, 5, request(0, 0, 0, 1), 16) == -22 && t.s(54, fd, 263, 5, request(0, 0, 0, 0), 16) == 0 && ring_of(fd, 0, &count).empty() && count == 0);

  // TPACKET_V3: blocks, handed over when they are full or have waited (here 20 ms)
  CHECK(t.s(54, fd, 1, 27, t.word(0), 4) == 0 && t.s(54, fd, 263, 10, t.word(2), 4) == 0 && t.s(54, fd, 263, 5, request(4096, 2, 2048, 4, 20), 16) == -22 &&
        t.s(54, fd, 263, 5, request(4096, 2, 2048, 4, 20), 28) == 0);
  ring = ring_of(fd, 0);
  CHECK(ring.size() == 8192 && arrives(fd));
  const uint32_t first = u32(ring, 16);
  CHECK(u32(ring, 0) == 2 && (u32(ring, 8) & 1) == 1 && u32(ring, 12) >= 1 && first == 48 && u32(ring, first + 20) == 1 && u16(ring, first + 24) == 82 && u16(ring, first + 26) == 96 &&
        rings::bytes(ring, first + 82, asked.size()) == asked && u32(ring, first + 12) >= asked.size());
  t.put32(size, 12);
  CHECK(t.s(55, fd, 263, 6, buffer, size) == 0 && t.u32(size) == 12 && t.u32(buffer) >= 1 && t.u32(buffer + 8) == 0);      // (struct tpacket_stats_v3)
  rings::put32(ring, 8, 0);
  CHECK(t.polled(fd, kPollIn) == 0);

  // A transmit ring: the program leaves frames in it, and send() sends them
  const int64_t out = t.s(41, kPacket, kRaw | kNonBlock, kAllProtocols);
  CHECK(out >= 3 && t.s(49, out, address, 20) == 0 && t.s(54, out, 263, 10, t.word(1), 4) == 0 && t.s(54, out, 263, 13, request(4096, 1, 2048, 2), 16) == 0);
  ByteView transmit = ring_of(out, 0, &count);
  CHECK(count == 1 && transmit.size() == 4096 && t.s(44, out, 0, 0, 0, 0, 0) == 0);      // (nothing left to send)
  const std::string frame = ethernet(everyone, mac_a, 0x0806, arp_message(1, mac_a, here, there));
  uint8_t seen[1514];
  network::FrameInfo info;
  while (other->receive(ByteView(seen), &info)) {}
  for (uint64_t slot = 0; slot < 4096; slot += 2048) {
    memcpy(transmit.data() + slot + 32, frame.data(), frame.size());
    rings::put32(transmit, slot + 4, uint32_t(frame.size()));
    rings::put32(transmit, slot, 1);      // TP_STATUS_SEND_REQUEST
  }
  CHECK(t.s(44, out, 0, 0, 0, 0, 0) == int64_t(2 * frame.size()) && u32(transmit, 0) == 0 && u32(transmit, 2048) == 0 && t.lan.run_until([&] { return other->pending() >= 2; }, 3000));
  CHECK(other->receive(ByteView(seen), &info) && info.length >= frame.size() && memcmp(seen, frame.data(), frame.size()) == 0 && other->receive(ByteView(seen), &info));
  // one that is too long is marked and the call fails; put right, it goes, and the one after it; with PACKET_LOSS such a frame is passed over
  rings::put32(transmit, 4, 1900); rings::put32(transmit, 0, 1);
  rings::put32(transmit, 2048 + 4, uint32_t(frame.size())); rings::put32(transmit, 2048, 1);
  CHECK(t.s(1, out, buffer, 0) == -90 && u32(transmit, 0) == 4 && u32(transmit, 2048) == 1);
  rings::put32(transmit, 4, uint32_t(frame.size())); rings::put32(transmit, 0, 1);
  CHECK(t.s(1, out, buffer, 0) == int64_t(2 * frame.size()) && u32(transmit, 0) == 0 && u32(transmit, 2048) == 0);
  rings::put32(transmit, 4, 1900); rings::put32(transmit, 0, 1);
  rings::put32(transmit, 2048 + 4, uint32_t(frame.size())); rings::put32(transmit, 2048, 1);
  CHECK(t.s(54, out, 263, 14, t.word(1), 4) == 0 && t.option(out, 263, 14, &v) == 0 && v == 1 && t.s(1, out, buffer, 0) == int64_t(frame.size()) && u32(transmit, 0) == 0 &&
        u32(transmit, 2048) == 0);
  // both rings on one socket: the receive ring comes first in what mmap() maps
  CHECK(t.s(54, out, 263, 5, request(4096, 2, 4096, 2), 16) == 0);
  CHECK(ring_of(out, 0, &count).size() == 8192 && count == 2 && ring_of(out, 1).size() == 4096 && ring_of(out, 1).data() == transmit.data());
  CHECK(t.s(3, out) == 0 && t.s(3, fd) == 0);
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
  test_control_messages();
  test_netlink_messages();
  test_netlink_sockets();
  test_raw_sockets();
  test_socket_filter();
  test_packet_sockets();
  test_packet_ring_formats();
  test_packet_ring_sockets();
  if (g_failed) { printf("LinuxInetSocketTest: %d check(s) failed\n", g_failed); return 1; }
  printf("LinuxInetSocketTest: all passed\n");
  return 0;
}
