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

// An in-memory network for the WebRTC tests: nodes with an address, UDP sockets by port, a STUN server, a NAT that rewrites the source of a node's packets.
#pragma once
#include <string.h>
#include <vector>
#include "Rtc/Ice.hpp"
#include "Rtc/RtcAudio.hpp"
#include "Rtc/Stun.hpp"

using network::Ipv4Address;
using namespace rtc;

struct FakeWire;
struct FakeUdpSocket final : network::IUdpSocket {
  struct Dg { std::vector<uint8_t> data; Ipv4Address from; uint16_t port; };
  FakeWire* wire = nullptr;
  Ipv4Address address{}, public_address{};   // public_address: what the outside sees (a NAT), if set
  uint16_t port_ = 0;
  std::vector<Dg> queue;
  bool closed = false;
  uint16_t local_port() const override { return port_; }
  bool send_to(Ipv4Address to, uint16_t port, ConstByteView data) override;
  bool receive_from(ByteView buffer, Ipv4Address* from, uint16_t* from_port, uint32_t* length) override {
    if (queue.empty()) return false;
    Dg d = queue.front();
    queue.erase(queue.begin());
    const size_t n = d.data.size() < buffer.size() ? d.data.size() : buffer.size();
    if (n) memcpy(buffer.data(), d.data.data(), n);
    *from = d.from; *from_port = d.port; *length = static_cast<uint32_t>(d.data.size());
    return true;
  }
  uint32_t pending() const override { return static_cast<uint32_t>(queue.size()); }
  void close() override { closed = true; }
};
struct FakeWire {
  allocators::IAllocator* alloc = nullptr;
  std::vector<FakeUdpSocket*> sockets;
  uint64_t now = 1000;
  bool drop_all = false;
  Ipv4Address stun_address = Ipv4Address::make(198, 51, 100, 1);
  FakeUdpSocket* find(Ipv4Address a, uint16_t port) {
    for (FakeUdpSocket* s : sockets)
      if (!s->closed && ((s->address == a && s->public_address.is_unspecified()) || (!s->public_address.is_unspecified() && s->public_address == a)) && s->port_ == port) return s;
    return nullptr;
  }
};
bool FakeUdpSocket::send_to(Ipv4Address to, uint16_t port, ConstByteView data) {
  if (wire->drop_all) return true;
  const Ipv4Address source = public_address.is_unspecified() ? address : public_address;
  if (to == wire->stun_address) {   // the STUN server: answers with the source it sees
    StunMessage m(*wire->alloc);
    if (!stun_parse(data, &m) || m.type != kStunBindingRequest) return true;
    ByteBuffer out(*wire->alloc);
    stun_begin(&out, kStunBindingSuccess, m.transaction);
    stun_xor_address(&out, kStunXorMapped, source, port_, m.transaction);
    stun_finish(&out, ConstByteView(), true);
    Dg d;
    d.data.assign(out.view().data(), out.view().data() + out.size());
    d.from = to;
    d.port = port;
    queue.push_back(d);
    return true;
  }
  FakeUdpSocket* dest = wire->find(to, port);
  if (!dest) return true;
  Dg d;
  d.data.assign(data.data(), data.data() + data.size());
  d.from = source;
  d.port = port_;
  dest->queue.push_back(d);
  return true;
}

struct FakeNode final : IRtcNetwork {
  FakeNode(FakeWire& w, allocators::IAllocator& a, Ipv4Address address, uint8_t seed) : wire(w), alloc(a), addr(address), state(seed) {}
  ~FakeNode() { for (auto& s : owned) static_cast<FakeUdpSocket*>(s.get())->closed = true; }
  FakeWire& wire;
  allocators::IAllocator& alloc;
  Ipv4Address addr, nat{};
  uint8_t state;
  uint16_t next_port = 40000;
  std::vector<SharedPointer<network::IUdpSocket>> owned;
  SharedPointer<network::IUdpSocket> open_udp(uint16_t) override {
    auto s = SharedPointer<network::IUdpSocket>::make_as<FakeUdpSocket>(alloc);
    FakeUdpSocket* f = static_cast<FakeUdpSocket*>(s.get());
    f->wire = &wire;
    f->address = addr;
    f->public_address = nat;
    f->port_ = static_cast<uint16_t>(next_port++ + addr.octets[3] * 100);
    wire.sockets.push_back(f);
    owned.push_back(s);
    return s;
  }
  bool local_address(Ipv4Address* a) override { *a = addr; return true; }
  bool resolve(StringView host, Ipv4Address* out) override {
    if (host.equals("stun.test")) { *out = wire.stun_address; return true; }
    return false;
  }
  uint64_t now_ms() override { return wire.now; }
  void poll() override {}
  bool random(ByteView out) override {
    for (uint64_t i = 0; i < out.size(); ++i) { state = static_cast<uint8_t>(state * 73 + 41 + i); out[i] = state; }
    return true;
  }
};


// Sound for the tests: a sine tone as the microphone (it produces what the wire's clock says has passed), a recording speaker, and the devices of a machine that has both.
struct SineSource final : IRtcAudioSource {
  SineSource(FakeWire& w, double hz) : wire(w), frequency(hz), last(w.now) {}
  FakeWire& wire;
  double frequency;
  uint64_t last, produced = 0;
  uint32_t read(ArrayView<int16_t> out) override {
    uint64_t want = (wire.now - last) * 8;   // 8 samples for each millisecond
    if (want > out.size()) want = out.size();
    last += want / 8;
    for (uint64_t i = 0; i < want; ++i) {
      const double phase = 6.283185307179586 * frequency * static_cast<double>(produced++) / 8000.0;
      out[i] = static_cast<int16_t>(8000.0 * __builtin_sin(phase));
    }
    return static_cast<uint32_t>(want);
  }
};
struct RecordingSink final : IRtcAudioSink {
  std::vector<int16_t>* samples;
  explicit RecordingSink(std::vector<int16_t>* s) : samples(s) {}
  void write(ConstByteView pcm) override {
    for (uint64_t i = 0; i + 1 < pcm.size(); i += 2) samples->push_back(static_cast<int16_t>(pcm[i] | (pcm[i + 1] << 8)));
  }
};
struct TestDevices final : IRtcAudioDevices {
  TestDevices(FakeWire& w, allocators::IAllocator& a) : wire(w), alloc(a) {}
  FakeWire& wire;
  std::vector<int16_t> heard;
  bool microphone_present() override { return present; }
  int permission = 1;   // 1 yes, 0 no, -1 not decided (the next request decides: `answer`)
  int answer = 1, requests = 0;
  bool present = true;
  int microphone_permission(StringView) override { return permission; }
  int microphone_request(StringView) override {
    ++requests;
    if (permission < 0 && requests >= 15) permission = answer;   // (the user takes a moment)
    return permission;
  }
  double tone = 440;
  UniquePointer<IRtcAudioSource> open_microphone() override {
    UniquePointer<SineSource> s = UniquePointer<SineSource>::make(alloc, wire, tone);
    return UniquePointer<IRtcAudioSource>(static_cast<UniquePointer<SineSource>&&>(s));
  }
  UniquePointer<IRtcAudioSink> open_speaker() override {
    UniquePointer<RecordingSink> sink = UniquePointer<RecordingSink>::make(alloc, &heard);
    return UniquePointer<IRtcAudioSink>(static_cast<UniquePointer<RecordingSink>&&>(sink));
  }
  allocators::IAllocator& alloc;
};
