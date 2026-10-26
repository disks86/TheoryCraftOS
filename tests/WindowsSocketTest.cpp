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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include "NetworkHarness.hpp"
#include "Process/Process.hpp"
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
static uint16_t swap16(uint16_t v) { return uint16_t((v << 8) | (v >> 8)); }

struct OneStack final : network::IStackProvider {
  network::INetworkStack* stack;
  explicit OneStack(network::INetworkStack* s) : stack(s) {}
  uint32_t count() const override { return 1; }
  network::INetworkStack* at(uint32_t index) const override { return index == 0 ? stack : nullptr; }
};

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
  uint32_t create_socket(bool stream, uint64_t* handle) {
    const uint32_t status = call(WindowsSyscallNumber::NtCreateSocket, 2 /*AF_INET*/, stream ? 1u : 2u, 0, kBase + 0x100);
    *handle = memory.get64(kBase + 0x100);
    return status;
  }
};

constexpr uint32_t kSuccess = 0, kStatusWouldBlock = 0xC0000103u;
// Matches network::load32/store32: the first octet is the top byte (network/big-endian order), the same layout as a raw sockaddr_in.sin_addr.
static uint32_t ip(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  return (uint32_t{a} << 24) | (uint32_t{b} << 16) | (uint32_t{c} << 8) | d;
}

static void test_tcp_client_server() {
  Setup s;
  SharedPointer<network::ITcpSocket> listener = s.lan.b.tcp_open(), peer;
  CHECK(listener->listen(9000));
  s.host.meanwhile = [&] { if (!peer) peer = listener->accept(); };

  uint64_t client = 0;
  CHECK(s.create_socket(true, &client) == kSuccess);
  CHECK(s.call(WindowsSyscallNumber::NtConnectSocket, client, ip(10, 0, 0, 2), swap16(9000)) == kSuccess);
  CHECK(peer && peer->is_connected());

  const char text[] = "hello from the client";
  s.memory.write(kBase + 0x200, ConstByteView(reinterpret_cast<const uint8_t*>(text), sizeof(text)));
  CHECK(s.call(WindowsSyscallNumber::NtSendSocket, client, kBase + 0x200, sizeof(text), kBase + 0x300, 0, 0, 0) == kSuccess);
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
  CHECK(s.call(WindowsSyscallNumber::NtReceiveSocket, client, kBase + 0x400, sizeof(reply), kBase + 0x300, 0, 0, 0) == kSuccess);
  CHECK(s.memory.get32(kBase + 0x300) == sizeof(reply));
  uint8_t back[64] = {};
  s.memory.read(kBase + 0x400, ByteView(back, sizeof(reply)));
  CHECK(!strcmp(reinterpret_cast<const char*>(back), reply));

  // getsockname / getpeername
  uint8_t info[8];
  CHECK(s.call(WindowsSyscallNumber::NtSocketControl, client, 2 /*GetSockName*/, 0, 0, kBase + 0x500, 8) == kSuccess);
  s.memory.read(kBase + 0x500, ByteView(info, 8));
  CHECK(info[0] == 10 && info[1] == 0 && info[2] == 0 && info[3] == 1);
  CHECK(s.call(WindowsSyscallNumber::NtSocketControl, client, 3 /*GetPeerName*/, 0, 0, kBase + 0x500, 8) == kSuccess);
  s.memory.read(kBase + 0x500, ByteView(info, 8));
  CHECK(info[0] == 10 && info[3] == 2 && info[4] == uint8_t(9000) && info[5] == uint8_t(9000 >> 8));

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
  const uint8_t one = 1;
  s.memory.write(kBase + 0x100, ConstByteView(&one, 1));
  CHECK(s.call(WindowsSyscallNumber::NtSocketControl, client, 1 /*SetNonBlocking*/, kBase + 0x100, 1, 0, 0) == kSuccess);
  // nothing is listening on 9999: a non-blocking receive on an unconnected socket would block, not fail oddly
  CHECK(s.call(WindowsSyscallNumber::NtConnectSocket, client, ip(10, 0, 0, 2), swap16(9999)) == kStatusWouldBlock);
  CHECK(s.call(WindowsSyscallNumber::NtAcceptSocket, client, kBase + 0x200) != kSuccess);      // not a listening socket
  uint64_t bad = client + 4;      // not a real handle
  CHECK(s.call(WindowsSyscallNumber::NtSendSocket, bad, 0, 0, 0, 0, 0, 0) == 0xC0000008u);      // STATUS_INVALID_HANDLE
}

static void test_udp_round_trip() {
  Setup s;
  SharedPointer<network::IUdpSocket> peer = s.lan.b.udp_open(9100);
  uint64_t client = 0;
  CHECK(s.create_socket(false, &client) == kSuccess);
  CHECK(s.call(WindowsSyscallNumber::NtBindSocket, client, 0, swap16(9050)) == kSuccess);
  uint8_t bound_info[8];
  CHECK(s.call(WindowsSyscallNumber::NtSocketControl, client, 2 /*GetSockName*/, 0, 0, kBase + 0x700, 8) == kSuccess);
  s.memory.read(kBase + 0x700, ByteView(bound_info, 8));
  CHECK(bound_info[4] == uint8_t(9050) && bound_info[5] == uint8_t(9050 >> 8));
  const char text[] = "a datagram";
  s.memory.write(kBase + 0x200, ConstByteView(reinterpret_cast<const uint8_t*>(text), sizeof(text)));
  const uint8_t has_address = 1;
  CHECK(s.call(WindowsSyscallNumber::NtSendSocket, client, kBase + 0x200, sizeof(text), kBase + 0x300, ip(10, 0, 0, 2), swap16(9100), has_address) == kSuccess);
  CHECK(s.lan.run_until([&] { return peer->pending() > 0; }, 2000));
  uint8_t buffer[32] = {};
  network::Ipv4Address from;
  uint16_t from_port = 0;
  uint32_t length = 0;
  CHECK(peer->receive_from(ByteView(buffer, sizeof(buffer)), &from, &from_port, &length) && length == sizeof(text) && !strcmp(reinterpret_cast<char*>(buffer), text));
  CHECK(from == network::Ipv4Address::make(10, 0, 0, 1));

  // connect() on UDP remembers the peer, so plain send()/recv() work without an address each time
  uint64_t connected = 0;
  CHECK(s.create_socket(false, &connected) == kSuccess);
  CHECK(s.call(WindowsSyscallNumber::NtConnectSocket, connected, ip(10, 0, 0, 2), swap16(9100)) == kSuccess);
  s.memory.write(kBase + 0x200, ConstByteView(reinterpret_cast<const uint8_t*>(text), sizeof(text)));
  CHECK(s.call(WindowsSyscallNumber::NtSendSocket, connected, kBase + 0x200, sizeof(text), 0, 0, 0, 0) == kSuccess);
  CHECK(s.lan.run_until([&] { return peer->pending() > 0; }, 2000));
  peer->receive_from(ByteView(buffer, sizeof(buffer)), &from, &from_port, &length);
  CHECK(length == sizeof(text));
  CHECK(peer->send_to(from, from_port, ConstByteView(reinterpret_cast<const uint8_t*>("reply"), 6)));
  CHECK(s.lan.run_until([&] {
    uint32_t got = 0;
    const uint32_t status = s.call(WindowsSyscallNumber::NtReceiveSocket, connected, kBase + 0x400, sizeof(buffer), kBase + 0x300, 0, 0, 0);
    got = s.memory.get32(kBase + 0x300);
    return status == kSuccess && got == 6;
  }));
}

static void test_select() {
  Setup s;
  SharedPointer<network::ITcpSocket> listener = s.lan.b.tcp_open(), peer;
  listener->listen(9200);
  s.host.meanwhile = [&] { if (!peer) peer = listener->accept(); };
  uint64_t client = 0;
  s.create_socket(true, &client);
  s.call(WindowsSyscallNumber::NtConnectSocket, client, ip(10, 0, 0, 2), swap16(9200));
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

int main() {
  test_tcp_client_server();
  test_tcp_non_blocking_and_errors();
  test_udp_round_trip();
  test_select();
  if (g_failed) { printf("WindowsSocketTest: %d failed\n", g_failed); return 1; }
  printf("WindowsSocketTest: all passed\n");
  return 0;
}
