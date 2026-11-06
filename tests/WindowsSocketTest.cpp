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

// Host tests of the Windows socket services (src/Process/Sockets.{hpp,cpp}, src/Process/WindowsSockets.cpp): one simulated machine (lan.a) is "this process", reached
// through the NT-ish socket services exactly as ws2_32.dll would call them; the other side of the wire (lan.b) is driven directly through the network stack's own API,
// standing in for a peer somewhere else on the network. A blocking service call's retry loop sleeps through the fake host, which steps the simulated wire and, if one is
// set, runs a test's own "what the peer does meanwhile" hook - the host test has no separate thread to run the peer on, unlike the real OS's network service thread.
// Covers IPv4 and IPv6 (global, link-local and loopback), dual-stack sockets, per-family listeners on one port, DNS lookups, and the socket address codec.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include "NetworkHarness.hpp"
#include "Ipv6Harness.hpp"
#include "Network/Dns/DnsMessage.hpp"
#include "Process/Process.hpp"
#include "Process/SocketAddress.hpp"
#include "Process/Sockets.hpp"
#include "Process/Syscall.hpp"
#include "Process/WindowsSyscalls.hpp"

using namespace process;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

constexpr uint64_t kBase = 0x100000, kSize = 0x4000;
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
  void put32(uint64_t a, uint32_t v) { for (int i = 0; i < 4; ++i) bytes[a - kBase + i] = uint8_t(v >> (8 * i)); }
  void put64(uint64_t a, uint64_t v) { for (int i = 0; i < 8; ++i) bytes[a - kBase + i] = uint8_t(v >> (8 * i)); }
  uint32_t get32(uint64_t a) { uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= uint32_t{bytes[a - kBase + i]} << (8 * i); return v; }
  uint64_t get64(uint64_t a) { uint64_t v = 0; for (int i = 0; i < 8; ++i) v |= uint64_t{bytes[a - kBase + i]} << (8 * i); return v; }
  uint8_t* at(uint64_t a) { return &bytes[a - kBase]; }
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
    if (++steps > 200000) { printf("test stuck waiting (no progress after many steps)\n"); exit(1); }
    if (lan) lan->step(int(ns / 1000000) + 1);
    if (meanwhile) meanwhile();
  }
  void yield() override { sleep_ns(0); }
};
struct Runtime final : IProcessRuntime {
  uint64_t current = 0;
  void lock() override {}
  void unlock() override {}
  bool wait(uint64_t) override { return false; }
  void notify() override {}
  bool interrupted() override { return false; }
  bool start_thread(const ThreadStart&, bool) override { return false; }
  bool resume_thread(uint64_t) override { return false; }
  uint64_t current_thread_id() override { return current; }
};

struct OneStack final : network::IStackProvider {
  network::INetworkStack* stack;
  explicit OneStack(network::INetworkStack* s) : stack(s) {}
  uint32_t count() const override { return 1; }
  network::INetworkStack* at(uint32_t index) const override { return index == 0 ? stack : nullptr; }
};

constexpr uint32_t kSuccess = 0, kStatusWouldBlock = 0xC0000103u, kInvalidParameter = 0xC000000Du, kInvalidAddress = 0xC0000141u, kAddressInUse = 0xC000020Au,
                   kBufferTooSmall = 0xC0000023u, kNotFound = 0xC0000225u, kLengthMismatch = 0xC0000004u, kAccessViolation = 0xC0000005u;
constexpr uint32_t kAfInet = 2, kAfInet6 = 23;

struct Setup {
  Lan lan;
  OneStack provider{&lan.a};
  SocketManager sockets{provider};
  FakeMemory memory;
  Host host;
  Runtime runtime;
  Process process{7, ProcessBinding{SyscallAbi::Windows, UserArch::X86_64, syscall_interface(SyscallAbi::Windows, UserArch::X86_64)}, memory, host};
  const ISyscallInterface* iface = syscall_interface(SyscallAbi::Windows, UserArch::X86_64);
  Setup() {
    lan.configure_static();
    set_system_sockets(&sockets);
    host.lan = &lan;
    process.attach_runtime(&runtime);
  }
  ~Setup() { set_system_sockets(nullptr); }

  uint32_t call(WindowsSyscallNumber n, uint64_t a0 = 0, uint64_t a1 = 0, uint64_t a2 = 0, uint64_t a3 = 0, uint64_t a4 = 0, uint64_t a5 = 0, uint64_t a6 = 0,
                uint64_t a7 = 0) {
    SyscallRequest r;
    r.number = uint64_t(n);
    r.args[0] = a0; r.args[1] = a1; r.args[2] = a2; r.args[3] = a3;
    const uint64_t sp = kBase + 0x1000;
    memory.put64(sp + 0x28, a4);
    memory.put64(sp + 0x30, a5);
    memory.put64(sp + 0x38, a6);
    memory.put64(sp + 0x40, a7);
    r.user_stack = sp;
    return uint32_t(iface->dispatch(process, r));
  }
  uint32_t create_socket(bool stream, uint64_t* handle, uint32_t family = kAfInet) {
    const uint32_t status = call(WindowsSyscallNumber::NtCreateSocket, family, stream ? 1u : 2u, 0, kBase + 0x100);
    *handle = memory.get64(kBase + 0x100);
    return status;
  }
  // A SOCKADDR_IN / SOCKADDR_IN6 in the process's memory at `at`; returns its size.
  uint32_t put_address(uint64_t at, const network::IpAddress& address, uint16_t port) {
    return write_socket_address(ByteView(memory.at(at), 28), address, port);
  }
  network::IpAddress get_address(uint64_t at, uint16_t* port) {
    network::IpAddress a;
    CHECK(parse_socket_address(ConstByteView(memory.at(at), 28), &a, port));
    return a;
  }
  // The services taking an address, with it written at kBase + 0xC00.
  uint32_t connect(uint64_t handle, const network::IpAddress& address, uint16_t port) {
    const uint32_t n = put_address(kBase + 0xC00, address, port);
    return call(WindowsSyscallNumber::NtConnectSocket, handle, kBase + 0xC00, n);
  }
  uint32_t bind(uint64_t handle, const network::IpAddress& address, uint16_t port) {
    const uint32_t n = put_address(kBase + 0xC00, address, port);
    return call(WindowsSyscallNumber::NtBindSocket, handle, kBase + 0xC00, n);
  }
  uint32_t send_to(uint64_t handle, ConstByteView data, const network::IpAddress& address, uint16_t port) {
    memory.write(kBase + 0x200, data);
    const uint32_t n = put_address(kBase + 0xC00, address, port);
    return call(WindowsSyscallNumber::NtSendSocket, handle, kBase + 0x200, data.size(), kBase + 0x300, kBase + 0xC00, n);
  }
  uint32_t set_flag(uint64_t handle, uint32_t operation, bool on) {
    const uint8_t v = on ? 1 : 0;
    memory.write(kBase + 0x180, ConstByteView(&v, 1));
    return call(WindowsSyscallNumber::NtSocketControl, handle, operation, kBase + 0x180, 1, 0, 0);
  }
  // GetSockName (2) / GetPeerName (3) into kBase + 0x500.
  uint32_t name(uint64_t handle, uint32_t operation, network::IpAddress* address, uint16_t* port) {
    memset(memory.at(kBase + 0x500), 0xEE, 28);
    const uint32_t status = call(WindowsSyscallNumber::NtSocketControl, handle, operation, 0, 0, kBase + 0x500, 28);
    if (status == kSuccess) *address = get_address(kBase + 0x500, port);
    return status;
  }
  // Waits until both machines have IPv6 link-local addresses, then gives them the 2001:db8:1::/64 prefix and a router.
  void configure_ipv6() {
    lan.run_until([&] { return lan.a.configuration6().link_local_ready && lan.b.configuration6().link_local_ready; }, 3000);
    lan.nic_a.inbox.push_back(router_advertisement(v6("2001:db8:1::"), 1800));
    lan.nic_b.inbox.push_back(router_advertisement(v6("2001:db8:1::"), 1800));
    lan.run_until([&] { return lan.a.configuration6().global_ready && lan.b.configuration6().global_ready; }, 3000);
  }
};

static network::IpAddress ip4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) { return network::IpAddress(network::Ipv4Address::make(a, b, c, d)); }
static network::IpAddress ip6(const char* text) { return network::IpAddress(v6(text)); }
static network::IpAddress mapped(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  return network::IpAddress(network::Ipv6Address::make(0, 0, 0, 0, 0, 0xFFFF, uint16_t(a << 8 | b), uint16_t(c << 8 | d)));
}
static ConstByteView text_view(const char* s) { return ConstByteView(reinterpret_cast<const uint8_t*>(s), strlen(s) + 1); }

// The codec for a program's SOCKADDR_IN / SOCKADDR_IN6: both round-trip; every truncation is refused, and bit flips either parse to something or are refused, never
// reading past the bytes given.
static void test_socket_address_codec() {
  uint8_t b[28];
  network::IpAddress a;
  uint16_t port = 0;
  CHECK(write_socket_address(ByteView(b), ip4(192, 168, 1, 7), 8080) == 16);
  CHECK(b[0] == 2 && b[1] == 0 && b[2] == 0x1F && b[3] == 0x90 && b[4] == 192 && b[7] == 7);
  CHECK(parse_socket_address(ConstByteView(b, 16), &a, &port) && a == ip4(192, 168, 1, 7) && port == 8080);
  for (uint32_t cut = 0; cut < 16; ++cut) CHECK(!parse_socket_address(ConstByteView(b, cut), &a, &port));
  CHECK(write_socket_address(ByteView(b), ip6("2001:db8::42"), 443) == 28);
  CHECK(b[0] == 23 && b[1] == 0 && b[2] == 1 && b[3] == 0xBB && b[8] == 0x20 && b[9] == 0x01 && b[23] == 0x42);
  CHECK(parse_socket_address(ConstByteView(b, 28), &a, &port) && a == ip6("2001:db8::42") && port == 443);
  for (uint32_t cut = 0; cut < 28; ++cut) CHECK(!parse_socket_address(ConstByteView(b, cut), &a, &port));
  CHECK(write_socket_address(ByteView(b, 27), ip6("::1"), 1) == 0);      // too small for it
  CHECK(write_socket_address(ByteView(b, 15), ip4(1, 2, 3, 4), 1) == 0);
  uint32_t parsed = 0;
  for (uint32_t bit = 0; bit < 28 * 8; ++bit) {
    write_socket_address(ByteView(b), ip6("fe80::1"), 7);
    b[bit / 8] ^= uint8_t(1u << (bit % 8));
    if (parse_socket_address(ConstByteView(b, 28), &a, &port)) ++parsed;
  }
  CHECK(parsed == 28 * 8 - 16);      // only a flipped family is refused (2 -> 23 is not one bit away)
  b[0] = 99; b[1] = 0;
  CHECK(!parse_socket_address(ConstByteView(b, 28), &a, &port));
}

static void test_tcp_client_server() {
  Setup s;
  SharedPointer<network::ITcpSocket> listener = s.lan.b.tcp_open(), peer;
  CHECK(listener->listen(9000));
  s.host.meanwhile = [&] { if (!peer) peer = listener->accept(); };

  uint64_t client = 0;
  CHECK(s.create_socket(true, &client) == kSuccess);
  CHECK(s.connect(client, ip4(10, 0, 0, 2), 9000) == kSuccess);
  CHECK(peer && peer->is_connected());

  const char text[] = "hello from the client";
  s.memory.write(kBase + 0x200, ConstByteView(reinterpret_cast<const uint8_t*>(text), sizeof(text)));
  CHECK(s.call(WindowsSyscallNumber::NtSendSocket, client, kBase + 0x200, sizeof(text), kBase + 0x300, 0, 0) == kSuccess);
  CHECK(s.memory.get32(kBase + 0x300) == sizeof(text));
  CHECK(s.lan.run_until([&] { return peer->available() >= sizeof(text); }, 2000));
  char got[64] = {};
  const uint32_t from_peer = peer->receive(ByteView(reinterpret_cast<uint8_t*>(got), sizeof(got)));
  CHECK(from_peer == sizeof(text) && !strcmp(got, text));

  const char reply[] = "hello from the server";
  bool reply_sent = false;
  s.host.meanwhile = [&] {
    if (reply_sent) return;    // sleep_ns can tick several times before the data arrives: send it once, not once per tick
    reply_sent = true;
    uint32_t sent = 0;
    while (sent < sizeof(reply)) sent += peer->send(ConstByteView(reinterpret_cast<const uint8_t*>(reply) + sent, sizeof(reply) - sent));
  };
  s.memory.put32(kBase + 0x300, 0);
  s.memory.put32(kBase + 0x340, 28);
  CHECK(s.call(WindowsSyscallNumber::NtReceiveSocket, client, kBase + 0x400, sizeof(reply), kBase + 0x300, kBase + 0x320, kBase + 0x340, 0) == kSuccess);
  CHECK(s.memory.get32(kBase + 0x300) == sizeof(reply));
  uint8_t back[64] = {};
  s.memory.read(kBase + 0x400, ByteView(back, sizeof(reply)));
  CHECK(!strcmp(reinterpret_cast<const char*>(back), reply));
  uint16_t port = 0;
  CHECK(s.memory.get32(kBase + 0x340) == 16 && s.get_address(kBase + 0x320, &port) == ip4(10, 0, 0, 2) && port == 9000);      // recvfrom on TCP: the peer

  // getsockname / getpeername: SOCKADDR_INs
  network::IpAddress a;
  CHECK(s.name(client, 2, &a, &port) == kSuccess && a == ip4(10, 0, 0, 1) && port == peer->remote_port());
  CHECK(s.name(client, 3, &a, &port) == kSuccess && a == ip4(10, 0, 0, 2) && port == 9000);
  CHECK(s.call(WindowsSyscallNumber::NtSocketControl, client, 3, 0, 0, kBase + 0x500, 15) == kLengthMismatch);

  // an orderly close is seen as a zero-byte, successful receive: a plain blocking receive's own retry loop waits for it (the fake host steps the wire while it does)
  s.host.meanwhile = nullptr;
  peer->close();
  s.memory.put32(kBase + 0x600, 0);
  const uint8_t zero = 0;
  s.memory.write(kBase + 0x610, ConstByteView(&zero, 1));
  CHECK(s.call(WindowsSyscallNumber::NtReceiveSocket, client, kBase + 0x400, sizeof(back), kBase + 0x600, 0, 0, kBase + 0x610) == kSuccess);
  CHECK(s.memory.get32(kBase + 0x600) == 0 && s.memory.bytes[0x610] != 0);
}

static void test_tcp_non_blocking_and_errors() {
  Setup s;
  uint64_t client = 0;
  CHECK(s.create_socket(true, &client) == kSuccess);
  CHECK(s.set_flag(client, 1 /*SetNonBlocking*/, true) == kSuccess);
  // nothing is listening on 9999: a non-blocking connect reports it would block, not fail oddly
  CHECK(s.connect(client, ip4(10, 0, 0, 2), 9999) == kStatusWouldBlock);
  CHECK(s.call(WindowsSyscallNumber::NtAcceptSocket, client, kBase + 0x200) != kSuccess);      // not a listening socket
  uint64_t bad = client + 4;      // not a real handle
  CHECK(s.call(WindowsSyscallNumber::NtSendSocket, bad, 0, 0, 0, 0, 0) == 0xC0000008u);      // STATUS_INVALID_HANDLE
  // addresses that are not ones: too short, an unknown family, out of the process's memory, IPv6 on an IPv4 socket
  uint64_t other = 0;
  CHECK(s.create_socket(true, &other) == kSuccess);
  s.put_address(kBase + 0xC00, ip4(10, 0, 0, 2), 80);
  CHECK(s.call(WindowsSyscallNumber::NtConnectSocket, other, kBase + 0xC00, 15) == kInvalidParameter);
  s.memory.bytes[0xC00] = 7;
  CHECK(s.call(WindowsSyscallNumber::NtConnectSocket, other, kBase + 0xC00, 16) == kInvalidParameter);
  CHECK(s.call(WindowsSyscallNumber::NtConnectSocket, other, kBase + kSize - 4, 16) == kAccessViolation);
  CHECK(s.call(WindowsSyscallNumber::NtConnectSocket, other, 0, 16) == kAccessViolation);
  CHECK(s.connect(other, ip6("2001:db8::2"), 80) == kInvalidAddress);
  CHECK(s.set_flag(other, 6 /*SetV6Only*/, false) == kInvalidParameter);      // not an IPv6 socket
  CHECK(s.call(WindowsSyscallNumber::NtCreateSocket, 99, 1, 0, kBase + 0x100) == kInvalidParameter);
}

static void test_udp_round_trip() {
  Setup s;
  SharedPointer<network::IUdpSocket> peer = s.lan.b.udp_open(9100);
  uint64_t client = 0;
  CHECK(s.create_socket(false, &client) == kSuccess);
  CHECK(s.bind(client, ip4(0, 0, 0, 0), 9050) == kSuccess);
  network::IpAddress a;
  uint16_t port = 0;
  CHECK(s.name(client, 2, &a, &port) == kSuccess && port == 9050);
  const char text[] = "a datagram";
  CHECK(s.send_to(client, ConstByteView(reinterpret_cast<const uint8_t*>(text), sizeof(text)), ip4(10, 0, 0, 2), 9100) == kSuccess);
  CHECK(s.lan.run_until([&] { return peer->pending() > 0; }, 2000));
  uint8_t buffer[32] = {};
  network::Ipv4Address from;
  uint16_t from_port = 0;
  uint32_t length = 0;
  CHECK(peer->receive_from(ByteView(buffer, sizeof(buffer)), &from, &from_port, &length) && length == sizeof(text) && !strcmp(reinterpret_cast<char*>(buffer), text));
  CHECK(from == network::Ipv4Address::make(10, 0, 0, 1) && from_port == 9050);

  // connect() on UDP remembers the peer, so plain send()/recv() work without an address each time
  uint64_t connected = 0;
  CHECK(s.create_socket(false, &connected) == kSuccess);
  CHECK(s.connect(connected, ip4(10, 0, 0, 2), 9100) == kSuccess);
  s.memory.write(kBase + 0x200, ConstByteView(reinterpret_cast<const uint8_t*>(text), sizeof(text)));
  CHECK(s.call(WindowsSyscallNumber::NtSendSocket, connected, kBase + 0x200, sizeof(text), 0, 0, 0) == kSuccess);
  CHECK(s.lan.run_until([&] { return peer->pending() > 0; }, 2000));
  peer->receive_from(ByteView(buffer, sizeof(buffer)), &from, &from_port, &length);
  CHECK(length == sizeof(text));
  CHECK(peer->send_to(from, from_port, ConstByteView(reinterpret_cast<const uint8_t*>("reply"), 6)));
  CHECK(s.lan.run_until([&] {
    s.memory.put32(kBase + 0x340, 28);
    const uint32_t status = s.call(WindowsSyscallNumber::NtReceiveSocket, connected, kBase + 0x400, sizeof(buffer), kBase + 0x300, kBase + 0x320, kBase + 0x340, 0);
    return status == kSuccess && s.memory.get32(kBase + 0x300) == 6;
  }));
  CHECK(s.memory.get32(kBase + 0x340) == 16 && s.get_address(kBase + 0x320, &port) == ip4(10, 0, 0, 2) && port == 9100);
}

static void test_select() {
  Setup s;
  SharedPointer<network::ITcpSocket> listener = s.lan.b.tcp_open(), peer;
  listener->listen(9200);
  s.host.meanwhile = [&] { if (!peer) peer = listener->accept(); };
  uint64_t client = 0;
  s.create_socket(true, &client);
  s.connect(client, ip4(10, 0, 0, 2), 9200);
  CHECK(peer);

  // nothing to read yet: select() with no timeout returns at once with zero ready (*ReadCount is read/write, like a real fd_set's own fd_count: 1 handle in, 0 out)
  s.memory.put64(kBase + 0x100, client);
  s.memory.put32(kBase + 0x120, 1);
  const uint64_t zero_timeout = 0;
  s.memory.put64(kBase + 0x180, zero_timeout);
  s.host.meanwhile = nullptr;
  CHECK(s.call(WindowsSyscallNumber::NtSelectSockets, kBase + 0x120, kBase + 0x100, 0, 0, 0, 0, kBase + 0x180, kBase + 0x190) == kSuccess);
  CHECK(s.memory.get32(kBase + 0x190) == 0 && s.memory.get32(kBase + 0x120) == 0);

  // once the peer sends something, select() reports the read set
  s.memory.put32(kBase + 0x120, 1);
  s.host.meanwhile = [&] { static bool sent = false; if (!sent) { peer->send(ConstByteView(reinterpret_cast<const uint8_t*>("hi"), 2)); sent = true; } };
  CHECK(s.call(WindowsSyscallNumber::NtSelectSockets, kBase + 0x120, kBase + 0x100, 0, 0, 0, 0, 0, kBase + 0x190) == kSuccess);      // Timeout 0 (null pointer): waits
  CHECK(s.memory.get32(kBase + 0x190) == 1 && s.memory.get32(kBase + 0x120) == 1 && s.memory.get64(kBase + 0x100) == client);
}

// Operation 5 lists the interfaces' IPv4 configuration (iphlpapi's adapter list): one interface here, then STATUS_NO_MORE_ENTRIES; a short buffer or index is refused.
// Operation 8 is the same for IPv6, 7 describes a socket, 10 says whether IPv6 reaches beyond the link.
static void test_interface_configuration() {
  Setup s;
  uint64_t sock = 0;
  CHECK(s.create_socket(false, &sock) == kSuccess);
  s.memory.put32(kBase + 0x200, 0);
  CHECK(s.call(WindowsSyscallNumber::NtSocketControl, sock, 5 /*InterfaceConfiguration*/, kBase + 0x200, 4, kBase + 0x500, 16) == kSuccess);
  uint8_t c[16];
  s.memory.read(kBase + 0x500, ByteView(c, 16));
  CHECK(c[0] == 10 && c[1] == 0 && c[2] == 0 && c[3] == 1);            // address
  CHECK(c[4] == 255 && c[5] == 255 && c[6] == 255 && c[7] == 0);       // netmask
  CHECK(c[8] == 10 && c[9] == 0 && c[10] == 0 && c[11] == 254);        // gateway
  CHECK(c[12] == 0 && c[13] == 0 && c[14] == 0 && c[15] == 0);         // no DNS server
  s.memory.put32(kBase + 0x200, 1);
  CHECK(s.call(WindowsSyscallNumber::NtSocketControl, sock, 5, kBase + 0x200, 4, kBase + 0x500, 16) == 0x8000001Au);
  s.memory.put32(kBase + 0x200, 0xFFFFFFFFu);
  CHECK(s.call(WindowsSyscallNumber::NtSocketControl, sock, 5, kBase + 0x200, 4, kBase + 0x500, 16) == 0x8000001Au);
  s.memory.put32(kBase + 0x200, 0);
  CHECK(s.call(WindowsSyscallNumber::NtSocketControl, sock, 5, kBase + 0x200, 4, kBase + 0x500, 15) == kLengthMismatch);
  CHECK(s.call(WindowsSyscallNumber::NtSocketControl, sock, 5, kBase + 0x200, 2, kBase + 0x500, 16) == kLengthMismatch);
  CHECK(s.call(WindowsSyscallNumber::NtSocketControl, sock, 5, kBase + 0x200, 4, kSize + kBase, 16) == kAccessViolation);      // an output the process cannot write

  // no IPv6 route before the router advertisement; after it the global address, router and DNS server are there
  CHECK(s.call(WindowsSyscallNumber::NtSocketControl, sock, 10 /*HasIpv6Route*/, 0, 0, kBase + 0x500, 1) == kSuccess && s.memory.bytes[0x500] == 0);
  s.configure_ipv6();
  CHECK(s.call(WindowsSyscallNumber::NtSocketControl, sock, 10, 0, 0, kBase + 0x500, 1) == kSuccess && s.memory.bytes[0x500] == 1);
  CHECK(s.call(WindowsSyscallNumber::NtSocketControl, sock, 8 /*InterfaceConfiguration6*/, kBase + 0x200, 4, kBase + 0x500, 72) == kSuccess);
  const uint8_t* c6 = s.memory.at(kBase + 0x500);
  const network::Ipv6Configuration expect = s.lan.a.configuration6();
  CHECK(!memcmp(c6, expect.link_local.bytes, 16) && !memcmp(c6 + 16, expect.global.bytes, 16) && !memcmp(c6 + 32, v6("fe80::1").bytes, 16));
  CHECK(!memcmp(c6 + 48, v6("2001:db8:1::53").bytes, 16) && c6[64] == 64 && c6[65] == 3 && s.memory.get32(kBase + 0x500 + 68) == 1500);
  CHECK(s.call(WindowsSyscallNumber::NtSocketControl, sock, 8, kBase + 0x200, 4, kBase + 0x500, 71) == kLengthMismatch);
  s.memory.put32(kBase + 0x200, 1);
  CHECK(s.call(WindowsSyscallNumber::NtSocketControl, sock, 8, kBase + 0x200, 4, kBase + 0x500, 72) == 0x8000001Au);

  uint64_t six = 0;
  CHECK(s.create_socket(true, &six, kAfInet6) == kSuccess);
  CHECK(s.call(WindowsSyscallNumber::NtSocketControl, six, 7 /*GetSocketInfo*/, 0, 0, kBase + 0x500, 12) == kSuccess);
  CHECK(s.memory.get32(kBase + 0x500) == kAfInet6 && s.memory.get32(kBase + 0x504) == 1 && s.memory.bytes[0x508] == 1);
  CHECK(s.call(WindowsSyscallNumber::NtSocketControl, sock, 7, 0, 0, kBase + 0x500, 12) == kSuccess);
  CHECK(s.memory.get32(kBase + 0x500) == kAfInet && s.memory.get32(kBase + 0x504) == 2 && s.memory.bytes[0x508] == 0);
  CHECK(s.call(WindowsSyscallNumber::NtSocketControl, sock, 7, 0, 0, kBase + 0x500, 11) == kLengthMismatch);
}

// A server and a client in one process over 127.0.0.1 (what Godot's game and editor debugger do): the connection never touches the wire, and both ends report
// the loopback address.
static void test_loopback() {
  Setup s;
  uint64_t ipv4_out = 0;  // (IPv6 neighbor discovery has its own business on the wire meanwhile)
  s.lan.wire.tap = [&](const std::vector<uint8_t>& f, int) { if (f.size() > 14 && f[12] == 0x08 && (f[13] == 0x00 || f[13] == 0x06)) ++ipv4_out; };
  uint64_t server = 0, client = 0;
  CHECK(s.create_socket(true, &server) == kSuccess && s.create_socket(true, &client) == kSuccess);
  CHECK(s.bind(server, ip4(0, 0, 0, 0), 9300) == kSuccess);
  CHECK(s.call(WindowsSyscallNumber::NtListenSocket, server, 4) == kSuccess);
  CHECK(s.set_flag(client, 1 /*SetNonBlocking*/, true) == kSuccess);
  const uint32_t connect = s.connect(client, ip4(127, 0, 0, 1), 9300);
  CHECK(connect == kSuccess || connect == kStatusWouldBlock);
  // the accept waits (the fake host polls the stack while it does), and sees the client at the loopback address
  s.memory.put32(kBase + 0x240, 16);
  CHECK(s.call(WindowsSyscallNumber::NtAcceptSocket, server, kBase + 0x200, kBase + 0x210, kBase + 0x240) == kSuccess);
  const uint64_t accepted = s.memory.get64(kBase + 0x200);
  uint16_t port = 0;
  CHECK(s.memory.get32(kBase + 0x240) == 16 && s.get_address(kBase + 0x210, &port) == ip4(127, 0, 0, 1));
  for (int i = 0; i < 50; ++i) s.lan.step();
  const char text[] = "over the loopback";
  s.memory.write(kBase + 0x300, ConstByteView(reinterpret_cast<const uint8_t*>(text), sizeof(text)));
  CHECK(s.call(WindowsSyscallNumber::NtSendSocket, client, kBase + 0x300, sizeof(text), kBase + 0x380, 0, 0) == kSuccess);
  s.memory.put32(kBase + 0x480, 0);
  CHECK(s.call(WindowsSyscallNumber::NtReceiveSocket, accepted, kBase + 0x400, sizeof(text), kBase + 0x480, 0, 0, 0) == kSuccess);
  uint8_t back[sizeof(text)] = {};
  s.memory.read(kBase + 0x400, ByteView(back, sizeof(text)));
  CHECK(s.memory.get32(kBase + 0x480) == sizeof(text) && !memcmp(back, text, sizeof(text)));
  network::IpAddress a;
  CHECK(s.name(client, 2, &a, &port) == kSuccess && a == ip4(127, 0, 0, 1));      // the client's own end is the loopback address too
  CHECK(s.name(server, 3, &a, &port) == 0xC0000238u);      // a listener has no peer
  CHECK(ipv4_out == 0);
  s.lan.wire.tap = nullptr;
}

// TCP and UDP over IPv6 with AF_INET6 sockets: to the peer's global address (both ends got theirs from the router advertisement), to its link-local one, and to ::1.
static void test_ipv6_sockets() {
  Setup s;
  s.configure_ipv6();
  const network::Ipv6Configuration ca = s.lan.a.configuration6(), cb = s.lan.b.configuration6();
  SharedPointer<network::ITcpSocket> listener = s.lan.b.tcp_open(), peer;
  CHECK(listener->listen(9400));
  s.host.meanwhile = [&] { if (!peer) peer = listener->accept(); };
  uint64_t client = 0;
  CHECK(s.create_socket(true, &client, kAfInet6) == kSuccess);
  CHECK(s.connect(client, network::IpAddress(cb.global), 9400) == kSuccess);
  CHECK(peer && peer->is_connected() && peer->remote_ip() == network::IpAddress(ca.global));
  network::IpAddress a;
  uint16_t port = 0;
  CHECK(s.name(client, 2, &a, &port) == kSuccess && a == network::IpAddress(ca.global) && port == peer->remote_port() && s.memory.bytes[0x500] == 23);
  CHECK(s.name(client, 3, &a, &port) == kSuccess && a == network::IpAddress(cb.global) && port == 9400);
  CHECK(s.call(WindowsSyscallNumber::NtSocketControl, client, 3, 0, 0, kBase + 0x500, 27) == kLengthMismatch);      // a SOCKADDR_IN6 does not fit
  // data both ways
  const char text[] = "over IPv6";
  s.memory.write(kBase + 0x200, text_view(text));
  CHECK(s.call(WindowsSyscallNumber::NtSendSocket, client, kBase + 0x200, sizeof(text), kBase + 0x300, 0, 0) == kSuccess);
  CHECK(s.lan.run_until([&] { return peer->available() >= sizeof(text); }, 2000));
  uint8_t got[32] = {};
  CHECK(peer->receive(ByteView(got)) == sizeof(text) && !strcmp(reinterpret_cast<char*>(got), text));
  CHECK(peer->send(text_view("back")) == 5);
  s.memory.put32(kBase + 0x340, 28);
  CHECK(s.call(WindowsSyscallNumber::NtReceiveSocket, client, kBase + 0x400, 32, kBase + 0x300, kBase + 0x320, kBase + 0x340, 0) == kSuccess);
  CHECK(s.memory.get32(kBase + 0x300) == 5 && s.memory.get32(kBase + 0x340) == 28 && s.get_address(kBase + 0x320, &port) == network::IpAddress(cb.global));
  // a recvfrom buffer too small for a SOCKADDR_IN6 is refused before anything is taken
  CHECK(peer->send(text_view("more")) == 5);
  s.lan.run_until([&] { return s.sockets.readable(uint32_t(s.process.files().token(client / 4 - 1))) >= 5; }, 2000);
  s.memory.put32(kBase + 0x340, 16);
  CHECK(s.call(WindowsSyscallNumber::NtReceiveSocket, client, kBase + 0x400, 32, kBase + 0x300, kBase + 0x320, kBase + 0x340, 0) == kBufferTooSmall);
  CHECK(s.call(WindowsSyscallNumber::NtReceiveSocket, client, kBase + 0x400, 32, kBase + 0x300, 0, 0, 0) == kSuccess && s.memory.get32(kBase + 0x300) == 5);

  // UDP to the peer's link-local address, and its answer back
  s.host.meanwhile = nullptr;
  SharedPointer<network::IUdpSocket> udp_peer = s.lan.b.udp_open(9450);
  uint64_t udp = 0;
  CHECK(s.create_socket(false, &udp, kAfInet6) == kSuccess);
  CHECK(s.send_to(udp, text_view("link-local"), network::IpAddress(cb.link_local), 9450) == kSuccess);
  CHECK(s.lan.run_until([&] { return udp_peer->pending() > 0; }, 2000));
  network::IpAddress from;
  uint16_t from_port = 0;
  uint32_t length = 0;
  CHECK(udp_peer->receive_from_ip(ByteView(got), &from, &from_port, &length) && length == 11 && from == network::IpAddress(ca.link_local));
  CHECK(udp_peer->send_to_ip(from, from_port, text_view("answer")));
  CHECK(s.lan.run_until([&] {
    s.memory.put32(kBase + 0x340, 28);
    return s.call(WindowsSyscallNumber::NtReceiveSocket, udp, kBase + 0x400, 32, kBase + 0x300, kBase + 0x320, kBase + 0x340, 0) == kSuccess &&
           s.memory.get32(kBase + 0x300) == 7;
  }));
  CHECK(s.get_address(kBase + 0x320, &port) == network::IpAddress(cb.link_local) && port == 9450);
  // a datagram that would not fit one IPv6 packet without fragmenting
  static uint8_t big[1453];
  CHECK(s.send_to(udp, ConstByteView(big, 1452), network::IpAddress(cb.global), 9450) == kSuccess);
  CHECK(s.send_to(udp, ConstByteView(big, 1453), network::IpAddress(cb.global), 9450) == kInvalidParameter);
  // an IPv6-only socket refuses IPv4 (mapped) addresses
  CHECK(s.send_to(udp, text_view("x"), mapped(10, 0, 0, 2), 9450) == kInvalidAddress);
}

// Over ::1: an AF_INET6 listener and an AF_INET one share a port (each takes its own version), a dual-stack listener (IPV6_V6ONLY off) takes both and shows IPv4
// clients mapped, and it cannot share its port with either.
static void test_ipv6_loopback_and_dual_stack() {
  Setup s;
  s.lan.run_until([&] { return s.lan.a.configuration6().link_local_ready; }, 3000);
  uint64_t server6 = 0, server4 = 0;
  CHECK(s.create_socket(true, &server6, kAfInet6) == kSuccess && s.create_socket(true, &server4) == kSuccess);
  CHECK(s.bind(server6, ip6("::"), 9500) == kSuccess && s.call(WindowsSyscallNumber::NtListenSocket, server6, 4) == kSuccess);
  CHECK(s.bind(server4, ip4(0, 0, 0, 0), 9500) == kSuccess && s.call(WindowsSyscallNumber::NtListenSocket, server4, 4) == kSuccess);
  CHECK(s.bind(server6, ip4(0, 0, 0, 0), 9501) == kInvalidAddress);      // (already bound anyway, but the family is checked first)
  auto accept_from = [&](uint64_t server, uint64_t client, const network::IpAddress& to, network::IpAddress* peer) {
    s.set_flag(client, 1 /*SetNonBlocking*/, true);
    const uint32_t c = s.connect(client, to, 9500);
    if (c != kSuccess && c != kStatusWouldBlock) return false;
    s.memory.put32(kBase + 0x240, 28);
    if (s.call(WindowsSyscallNumber::NtAcceptSocket, server, kBase + 0x200, kBase + 0x210, kBase + 0x240) != kSuccess) return false;
    uint16_t port = 0;
    *peer = s.get_address(kBase + 0x210, &port);
    return true;
  };
  uint64_t c6 = 0, c4 = 0;
  network::IpAddress peer;
  CHECK(s.create_socket(true, &c6, kAfInet6) == kSuccess && s.create_socket(true, &c4) == kSuccess);
  CHECK(accept_from(server6, c6, ip6("::1"), &peer) && peer == ip6("::1") && s.memory.get32(kBase + 0x240) == 28);
  CHECK(accept_from(server4, c4, ip4(127, 0, 0, 1), &peer) && peer == ip4(127, 0, 0, 1) && s.memory.get32(kBase + 0x240) == 16);
  network::IpAddress a;
  uint16_t port = 0;
  CHECK(s.name(c6, 2, &a, &port) == kSuccess && a == ip6("::1"));

  // dual stack: off before it is used, then an IPv4 client is seen as ::ffff:127.0.0.1; too late once bound
  uint64_t dual = 0;
  CHECK(s.create_socket(true, &dual, kAfInet6) == kSuccess);
  CHECK(s.set_flag(dual, 6 /*SetV6Only*/, false) == kSuccess);
  CHECK(s.bind(dual, ip6("::"), 9500) == kSuccess && s.call(WindowsSyscallNumber::NtListenSocket, dual, 4) == kAddressInUse);      // both versions are taken
  uint64_t dual2 = 0;
  CHECK(s.create_socket(true, &dual2, kAfInet6) == kSuccess && s.set_flag(dual2, 6, false) == kSuccess);
  CHECK(s.bind(dual2, ip6("::"), 9600) == kSuccess && s.call(WindowsSyscallNumber::NtListenSocket, dual2, 4) == kSuccess);
  CHECK(s.set_flag(dual2, 6, true) == kInvalidParameter);
  CHECK(s.call(WindowsSyscallNumber::NtSocketControl, dual2, 7 /*GetSocketInfo*/, 0, 0, kBase + 0x500, 12) == kSuccess && s.memory.bytes[0x508] == 0);
  uint64_t old4 = 0, new6 = 0;
  CHECK(s.create_socket(true, &old4) == kSuccess && s.create_socket(true, &new6, kAfInet6) == kSuccess);
  s.set_flag(old4, 1, true);
  s.set_flag(new6, 1, true);
  const uint32_t c1 = s.connect(old4, ip4(127, 0, 0, 1), 9600), c2 = s.connect(new6, ip6("::1"), 9600);
  CHECK((c1 == kSuccess || c1 == kStatusWouldBlock) && (c2 == kSuccess || c2 == kStatusWouldBlock));
  network::IpAddress first, second;
  for (network::IpAddress* p : {&first, &second}) {
    s.memory.put32(kBase + 0x240, 28);
    CHECK(s.call(WindowsSyscallNumber::NtAcceptSocket, dual2, kBase + 0x200, kBase + 0x210, kBase + 0x240) == kSuccess);
    *p = s.get_address(kBase + 0x210, &port);
    CHECK(s.memory.get32(kBase + 0x240) == 28);
  }
  CHECK((first == mapped(127, 0, 0, 1) && second == ip6("::1")) || (first == ip6("::1") && second == mapped(127, 0, 0, 1)));
  // the client side of dual stack: connecting to a mapped address reaches the IPv4 listener, and the peer is shown mapped
  uint64_t dual_client = 0;
  CHECK(s.create_socket(true, &dual_client, kAfInet6) == kSuccess && s.set_flag(dual_client, 6, false) == kSuccess);
  CHECK(accept_from(server4, dual_client, mapped(127, 0, 0, 1), &peer) && peer == ip4(127, 0, 0, 1));
  for (int i = 0; i < 20; ++i) s.lan.step();
  CHECK(s.name(dual_client, 3, &a, &port) == kSuccess && a == mapped(127, 0, 0, 1) && port == 9500);
  // a too-small accept buffer for an IPv6 listener is refused without losing the connection
  uint64_t late = 0;
  CHECK(s.create_socket(true, &late, kAfInet6) == kSuccess);
  s.set_flag(late, 1, true);
  s.connect(late, ip6("::1"), 9500);
  s.memory.put32(kBase + 0x240, 16);
  CHECK(s.call(WindowsSyscallNumber::NtAcceptSocket, server6, kBase + 0x200, kBase + 0x210, kBase + 0x240) == kBufferTooSmall);
  CHECK(s.call(WindowsSyscallNumber::NtAcceptSocket, server6, kBase + 0x200, 0, 0) == kSuccess);
}

// ResolveName asks the kernel's resolver (what getaddrinfo uses for names that are not addresses): A and AAAA records, NXDOMAIN, and requests that are not ones.
static void test_resolve_name() {
  Setup s;
  network::IpConfiguration ca = s.lan.a.configuration();
  ca.dns = network::Ipv4Address::make(10, 0, 0, 2);
  s.lan.a.configure(ca);
  DnsServer dns(s.lan.b, "example.com");
  dns.a = network::Ipv4Address::make(10, 0, 0, 80);
  dns.aaaa = v6("2001:db8:1::80");
  s.host.meanwhile = [&] { dns.serve(); };
  uint64_t probe = 0;
  CHECK(s.create_socket(false, &probe) == kSuccess);
  auto resolve = [&](uint32_t family, const char* name, uint32_t out_length = 16) {
    s.memory.put32(kBase + 0x800, family);
    const uint32_t n = uint32_t(strlen(name));
    memcpy(s.memory.at(kBase + 0x804), name, n);
    memset(s.memory.at(kBase + 0x900), 0, 16);
    return s.call(WindowsSyscallNumber::NtSocketControl, probe, 9 /*ResolveName*/, kBase + 0x800, 4 + n, kBase + 0x900, out_length);
  };
  CHECK(resolve(kAfInet, "example.com") == kSuccess);
  const uint8_t* out = s.memory.at(kBase + 0x900);
  CHECK(out[0] == 10 && out[1] == 0 && out[2] == 0 && out[3] == 80);
  CHECK(resolve(kAfInet6, "example.com") == kSuccess && !memcmp(out, v6("2001:db8:1::80").bytes, 16));
  CHECK(resolve(kAfInet, "nowhere.example") == kNotFound);
  CHECK(dns.queries >= 3);
  CHECK(resolve(kAfInet6, "example.com", 15) == kLengthMismatch);
  CHECK(resolve(99, "example.com") == kInvalidParameter);
  CHECK(s.call(WindowsSyscallNumber::NtSocketControl, probe, 9, kBase + 0x800, 4, kBase + 0x900, 16) == kLengthMismatch);      // no name
  CHECK(s.call(WindowsSyscallNumber::NtSocketControl, probe, 9, kBase + 0x800, 4 + 254, kBase + 0x900, 16) == kLengthMismatch);      // longer than a name can be
  CHECK(resolve(kAfInet, "bad..name") == kNotFound);
  // literals are answered without a query
  const uint32_t before = dns.queries;
  CHECK(resolve(kAfInet6, "2001:db8::7") == kSuccess && !memcmp(out, v6("2001:db8::7").bytes, 16) && dns.queries == before);
}

int main() {
  test_socket_address_codec();
  test_interface_configuration();
  test_loopback();
  test_tcp_client_server();
  test_tcp_non_blocking_and_errors();
  test_udp_round_trip();
  test_select();
  test_ipv6_sockets();
  test_ipv6_loopback_and_dual_stack();
  test_resolve_name();
  if (g_failed) { printf("WindowsSocketTest: %d failed\n", g_failed); return 1; }
  printf("WindowsSocketTest: all passed\n");
  return 0;
}
