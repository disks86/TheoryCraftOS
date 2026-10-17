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

// A two-machine test network shared by the host tests: two NetworkStacks joined by
// a fake wire (with optional loss, duplication and reordering) and a fake clock.
// Whenever one stack's NIC has nothing to receive it lets the other machine run, so
// a blocking call on one side makes progress without threads.
#pragma once
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <string>
#include <vector>
#include "Network/NetworkStack.hpp"

using namespace network;

static long g_live = 0;
struct TestAllocator final : allocators::IAllocator {
  long& live = g_live;
  void* allocate(uint64_t size) override { if (!size) return nullptr; ++live; return aligned_alloc(64, (size + 63) / 64 * 64); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    if (!size) return nullptr;
    ++live;
    if (alignment < 64) alignment = 64;
    return aligned_alloc(alignment, (size + alignment - 1) / alignment * alignment);
  }
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


// A scripted TCP server on stack B. Its step() runs whenever stack A is idle (A's NIC has nothing to receive),
// so a blocking or awaiting client makes the server progress. `app` is the script; `received` collects what arrived.
struct Server {
  Lan& lan;
  std::function<void(Server&)> app;       // what the server does each time the client waits
  SharedPointer<ITcpSocket> listener, conn;
  std::string received;
  bool running = false;
  Server(Lan& l, uint16_t port) : lan(l) {
    listener = lan.b.tcp_open(); assert(listener->listen(port));
    auto previous = lan.nic_a.on_idle;
    lan.nic_a.on_idle = [this, previous] { if (previous) previous(); if (running) return; running = true; step(); running = false; };
  }
  void step() {
    if (!conn) conn = listener->accept();
    if (conn) { uint8_t b[1024]; for (uint32_t n; (n = conn->receive(ByteView(b))) > 0;) received.append(reinterpret_cast<char*>(b), n); }
    if (app) app(*this);
  }
  void send_all(const char* s) { ConstByteView v(reinterpret_cast<const uint8_t*>(s), strlen(s)); while (!v.empty()) { uint32_t k = conn->send(v); v = v.subview(k); if (!k) lan.step(); } }
};

