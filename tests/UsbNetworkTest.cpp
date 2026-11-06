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

// The USB Ethernet drivers against a fake host controller that plays the device: a CDC-ECM adapter (like QEMU's usb-net), a Realtek
// RTL8153-like one whose ECM function is only in its second configuration, a CDC-NCM one and an ASIX AX88179. Plus the parsers on their own,
// with truncated and bit-flipped input.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <deque>
#include <map>
#include <vector>
#include "Hardware/NetworkInterface/Usb/Ax88179NetworkInterface.hpp"
#include "Hardware/NetworkInterface/Usb/CdcEthernetInterface.hpp"
#include "Hardware/NetworkInterface/Usb/CdcNcm.hpp"
#include "Hardware/NetworkInterface/Usb/UsbNetworkCommon.hpp"
#include "Hardware/NetworkInterface/Usb/UsbNetworkInterfaces.hpp"

namespace hardware {
void delay_us(uint64_t) {}
}  // namespace hardware

using namespace hardware;

static int g_failed = 0, g_checks = 0;
#define CHECK(...) do { ++g_checks; if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

typedef std::vector<uint8_t> Bytes;

struct TestAllocator final : allocators::IAllocator {
  void* allocate(uint64_t size) override { return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    const uint64_t al = alignment < 16 ? 16 : alignment;
    return aligned_alloc(al, (size + al - 1) / al * al);
  }
  void deallocate(void* p) override { free(p); }
};

static void put16(Bytes& b, uint16_t v) { b.push_back(static_cast<uint8_t>(v)); b.push_back(static_cast<uint8_t>(v >> 8)); }
static void put32(Bytes& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i))); }
static uint16_t get16(const Bytes& b, size_t at) { return static_cast<uint16_t>(b[at] | (b[at + 1] << 8)); }
static uint32_t get32(const Bytes& b, size_t at) { return get16(b, at) | (uint32_t{get16(b, at + 2)} << 16); }
static void add(Bytes& b, std::initializer_list<uint8_t> v) { b.insert(b.end(), v.begin(), v.end()); }
static ConstByteView view(const Bytes& b) { return ConstByteView(b.data(), b.size()); }

static Bytes frame(size_t length, uint8_t seed) {
  Bytes f(length);
  for (size_t i = 0; i < length; ++i) f[i] = static_cast<uint8_t>(seed + i * 7);
  return f;
}

// ---- descriptors ----------------------------------------------------------------------------------------------------------------------------

static void finish_configuration(Bytes& c) { c[2] = static_cast<uint8_t>(c.size()); c[3] = static_cast<uint8_t>(c.size() >> 8); }

// A CDC Ethernet function (subclass 6 ECM or 13 NCM) as configuration `value`: interface 0 communication, interface 1 data (alternate 1 has
// the bulk endpoints).
static Bytes cdc_configuration(uint8_t value, uint8_t subclass, uint16_t bulk_packet, bool with_notify = true) {
  Bytes c;
  add(c, {9, 2, 0, 0, 2, value, 0, 0x80, 50});
  add(c, {9, 4, 0, 0, static_cast<uint8_t>(with_notify ? 1 : 0), 2, subclass, 0, 0});
  add(c, {5, 0x24, 0x00, 0x10, 0x01});                       // header
  add(c, {5, 0x24, 0x06, 0, 1});                             // union: control 0, data 1
  add(c, {13, 0x24, 0x0F, 3, 0, 0, 0, 0, 0xEA, 0x05, 0, 0, 0});   // Ethernet: iMACAddress 3, 1514
  if (subclass == 0x0D) add(c, {6, 0x24, 0x1A, 0x00, 0x01, 0x00});
  if (with_notify) add(c, {7, 5, 0x83, 3, 16, 0, 9});
  add(c, {9, 4, 1, 0, 0, 0x0A, 0, subclass == 0x0D ? uint8_t{1} : uint8_t{0}, 0});
  add(c, {9, 4, 1, 1, 2, 0x0A, 0, subclass == 0x0D ? uint8_t{1} : uint8_t{0}, 0});
  add(c, {7, 5, 0x81, 2, static_cast<uint8_t>(bulk_packet), static_cast<uint8_t>(bulk_packet >> 8), 0});
  add(c, {7, 5, 0x02, 2, static_cast<uint8_t>(bulk_packet), static_cast<uint8_t>(bulk_packet >> 8), 0});
  finish_configuration(c);
  return c;
}

static Bytes vendor_configuration(uint8_t value, uint16_t bulk_packet) {
  Bytes c;
  add(c, {9, 2, 0, 0, 1, value, 0, 0x80, 50});
  add(c, {9, 4, 0, 0, 3, 0xFF, 0xFF, 0, 0});
  add(c, {7, 5, 0x81, 3, 8, 0, 11});
  add(c, {7, 5, 0x82, 2, static_cast<uint8_t>(bulk_packet), static_cast<uint8_t>(bulk_packet >> 8), 0});
  add(c, {7, 5, 0x03, 2, static_cast<uint8_t>(bulk_packet), static_cast<uint8_t>(bulk_packet >> 8), 0});
  finish_configuration(c);
  return c;
}

static Bytes storage_configuration(uint8_t value) {
  Bytes c;
  add(c, {9, 2, 0, 0, 1, value, 0, 0x80, 50});
  add(c, {9, 4, 0, 0, 2, 0x08, 0x06, 0x50, 0});
  add(c, {7, 5, 0x81, 2, 0, 2, 0});
  add(c, {7, 5, 0x02, 2, 0, 2, 0});
  finish_configuration(c);
  return c;
}

static Bytes mac_string(const char* hex) {
  Bytes s;
  s.push_back(static_cast<uint8_t>(2 + 2 * strlen(hex)));
  s.push_back(3);
  for (const char* p = hex; *p; ++p) put16(s, static_cast<uint8_t>(*p));
  return s;
}

// ---- the fake host and device ---------------------------------------------------------------------------------------------------------------

struct FakeNicHost final : IUsbHostController {
  enum class Kind { Cdc, Ax88179 };
  Kind device = Kind::Cdc;
  uint16_t vendor = 0x0525, product = 0xA4A2;
  std::vector<Bytes> configurations;                           // by index; [cached] is what enumeration read
  size_t cached = 0;
  Bytes mac = mac_string("525400123456");
  uint32_t generation = 1;
  bool stream_supported = true;
  int set_configuration_value = -1;
  int data_alternate = -1;
  int packet_filter = -1;
  uint32_t ntb_input_size = 0;
  bool ntb_input_size_set = false;
  std::map<uint8_t, std::deque<Bytes>> in;                     // per IN endpoint: pieces waiting to be read
  std::vector<Bytes> sent;                                     // bulk OUT transfers
  int fail_bulk_out = 0;
  // AX88179: its MAC registers and PHY registers.
  std::map<uint16_t, Bytes> mac_registers;
  std::map<uint16_t, uint16_t> phy;

  const char* name() const override { return "fake nic host"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  UsbControllerKind kind() const override { return UsbControllerKind::Xhci; }
  uint32_t port_count() const override { return 1; }
  bool port_info(uint32_t port, UsbPortInfo* out) const override { out->connected = port == 0; out->speed = UsbSpeed::Super; return true; }
  bool device_descriptor(uint32_t port, UsbDeviceDescriptor* out) const override {
    if (port != 0) return false;
    memset(out, 0, sizeof *out);
    out->length = 18; out->descriptor_type = 1; out->vendor_id = vendor; out->product_id = product;
    out->configuration_count = static_cast<uint8_t>(configurations.size());
    return true;
  }
  bool configuration_descriptor(uint32_t port, ByteView buffer, uint16_t* length) const override {
    if (port != 0 || cached >= configurations.size()) return false;
    const Bytes& c = configurations[cached];
    const size_t n = c.size() < buffer.size() ? c.size() : buffer.size();
    buffer.first(n).copy_from(ConstByteView(c.data(), n));
    *length = static_cast<uint16_t>(n);
    return true;
  }
  uint32_t port_generation(uint32_t) const override { return generation; }
  bool set_configuration(uint32_t, uint8_t configuration) override { set_configuration_value = configuration; return true; }
  bool control_out(uint32_t, uint8_t type, uint8_t request, uint16_t value, uint16_t index) override {
    if (type == 0x01 && request == 0x0B) { if (index == 1) data_alternate = value; return true; }
    if (type == 0x21 && request == 0x43) { packet_filter = value; return true; }
    if (type == 0x21 && request == 0x84) return true;
    if (type == 0x02 && request == 0x01) return true;
    return false;
  }
  static bool answer(ByteView buffer, const Bytes& data) {
    buffer.fill(0);
    buffer.copy_from(ConstByteView(data.data(), data.size()).at_most(buffer.size()));
    return true;
  }
  bool control_in(uint32_t, uint8_t type, uint8_t request, uint16_t value, uint16_t index, ByteView buffer) override {
    if (type == 0x80 && request == 6 && (value >> 8) == 2) {
      if ((value & 0xFF) >= configurations.size()) return false;
      return answer(buffer, configurations[value & 0xFF]);
    }
    if (type == 0x80 && request == 6 && value == 0x0303 && index == 0x0409) return answer(buffer, mac);
    if (type == 0xA1 && request == 0x80) {                     // GET_NTB_PARAMETERS
      Bytes p;
      put16(p, 28); put16(p, 1); put32(p, 32768); put16(p, 4); put16(p, 0); put16(p, 4); put16(p, 0);
      put32(p, 16384); put16(p, 4); put16(p, 2); put16(p, 4); put16(p, 1);
      return answer(buffer, p);
    }
    if (device == Kind::Ax88179 && type == 0xC0 && request == 1) {
      auto it = mac_registers.find(value);
      Bytes v = it == mac_registers.end() ? Bytes(index) : it->second;
      if (v.size() != index) v.resize(index);
      return answer(buffer, v);
    }
    if (device == Kind::Ax88179 && type == 0xC0 && request == 2 && value == 3) {
      Bytes v;
      put16(v, phy[index]);
      return answer(buffer, v);
    }
    return false;
  }
  bool control_out_data(uint32_t, uint8_t type, uint8_t request, uint16_t value, uint16_t index, ConstByteView data) override {
    if (type == 0x21 && request == 0x86 && data.size() == 4) {
      Bytes b(data.begin(), data.end());
      ntb_input_size = get32(b, 0);
      ntb_input_size_set = true;
      return true;
    }
    if (device == Kind::Ax88179 && type == 0x40 && request == 1 && data.size() == index) { mac_registers[value] = Bytes(data.begin(), data.end()); return true; }
    if (device == Kind::Ax88179 && type == 0x40 && request == 2 && value == 3 && data.size() == 2) {
      uint16_t v = static_cast<uint16_t>(data[0] | (data[1] << 8));
      if (index == 0) v &= ~0x0200;                            // (restart is self-clearing)
      phy[index] = v;
      return true;
    }
    return false;
  }
  bool start_interrupt_in(uint32_t, uint8_t, uint16_t, uint8_t) override { return true; }
  bool poll_interrupt_in(uint32_t, uint8_t endpoint, ByteView buffer, uint16_t* length) override {
    std::deque<Bytes>& q = in[endpoint];
    if (q.empty()) return false;
    Bytes piece = q.front();
    q.pop_front();
    buffer.copy_from(ConstByteView(piece.data(), piece.size()).at_most(buffer.size()));
    *length = static_cast<uint16_t>(piece.size());
    return true;
  }
  bool start_bulk_in_stream(uint32_t, uint8_t, uint16_t) override { return stream_supported; }
  bool start_bulk_endpoint(uint32_t, uint8_t, uint16_t) override { return true; }
  bool bulk_transfer(uint32_t, uint8_t endpoint, ByteView data, uint32_t* transferred) override {
    if (endpoint & 0x80) return false;
    if (fail_bulk_out > 0) { --fail_bulk_out; return false; }
    sent.push_back(Bytes(data.begin(), data.end()));
    if (transferred) *transferred = static_cast<uint32_t>(data.size());
    return true;
  }
  bool reset_bulk_endpoint(uint32_t, uint8_t) override { return true; }

  // A transfer from the device, in pieces of the endpoint's packet size (a zero-length one after a transfer that is whole packets).
  void deliver(uint8_t endpoint, const Bytes& transfer, uint16_t max_packet) {
    std::deque<Bytes>& q = in[endpoint];
    for (size_t at = 0; at < transfer.size(); at += max_packet) q.push_back(Bytes(transfer.begin() + at, transfer.begin() + std::min(transfer.size(), at + size_t{max_packet})));
    if (transfer.size() % max_packet == 0) q.push_back(Bytes());
  }
};

static Bytes received(INetworkInterface& nic, bool* got) {
  uint8_t buffer[2048];
  uint32_t length = 0;
  *got = nic.receive(ByteView(buffer), &length);
  return *got ? Bytes(buffer, buffer + (length < sizeof buffer ? length : sizeof buffer)) : Bytes();
}

// ---- CDC-ECM --------------------------------------------------------------------------------------------------------------------------------

static void test_ecm() {
  TestAllocator a;
  FakeNicHost host;
  host.configurations.push_back(cdc_configuration(1, 0x06, 512));
  UniquePointer<INetworkInterface> nic = open_usb_network_interface(host, 0, a);
  CHECK(nic);
  if (!nic) return;
  CHECK(strcmp(nic->name(), "USB Ethernet (CDC-ECM)") == 0);
  const MacAddress want{{0x52, 0x54, 0x00, 0x12, 0x34, 0x56}};
  CHECK(nic->mac_address() == want);
  CHECK(host.set_configuration_value == 1 && host.data_alternate == 1 && host.packet_filter == 0x0C);
  CHECK(nic->link_up());                                       // (no notification yet: up)

  // Link down, then the speed (split over two interrupt packets) and up again.
  host.in[0x83].push_back(Bytes{0xA1, 0x00, 0, 0, 0, 0, 0, 0});
  CHECK(!nic->link_up() && nic->link_speed_mbps() == 0);
  Bytes speed{0xA1, 0x2A, 0, 0, 0, 0, 8, 0};
  put32(speed, 1000000000); put32(speed, 1000000000);
  host.in[0x83].push_back(Bytes(speed.begin(), speed.begin() + 8));
  host.in[0x83].push_back(Bytes(speed.begin() + 8, speed.end()));
  host.in[0x83].push_back(Bytes{0xA1, 0x00, 1, 0, 0, 0, 0, 0});
  CHECK(nic->link_up() && nic->link_speed_mbps() == 1000);
  host.in[0x83].push_back(Bytes{0x12, 0x34, 0x56});          // garbage: ignored
  CHECK(nic->link_up());

  // Transmit: as is, padded to 60, and one byte more when the frame is whole packets.
  const Bytes f100 = frame(100, 1), f42 = frame(42, 2), f512 = frame(512, 3);
  CHECK(nic->transmit(view(f100)) && nic->transmit(view(f42)) && nic->transmit(view(f512)));
  CHECK(host.sent.size() == 3 && host.sent[0] == f100);
  CHECK(host.sent[1].size() == 60 && Bytes(host.sent[1].begin(), host.sent[1].begin() + 42) == f42 && host.sent[1][59] == 0);
  CHECK(host.sent[2].size() == 513 && Bytes(host.sent[2].begin(), host.sent[2].begin() + 512) == f512 && host.sent[2][512] == 0);
  CHECK(!nic->transmit(ConstByteView()) && !nic->transmit(view(frame(1515, 0))));
  host.fail_bulk_out = 1;
  CHECK(!nic->transmit(view(f100)) && nic->transmit(view(f100)));
  CHECK(nic->statistics().frames_sent == 4 && nic->statistics().send_failures == 3);

  // Receive: a frame in three packets, one of exactly two packets (ended by a zero-length one), a runt, an oversized transfer, a good one.
  bool got = false;
  CHECK(received(*nic, &got).empty() && !got);
  const Bytes r1514 = frame(1514, 4), r1024 = frame(1024, 5), r60 = frame(60, 6);
  host.deliver(0x81, r1514, 512);
  host.deliver(0x81, r1024, 512);
  host.deliver(0x81, frame(10, 7), 512);
  host.deliver(0x81, frame(3000, 8), 512);
  host.deliver(0x81, r60, 512);
  CHECK(received(*nic, &got) == r1514 && got);
  CHECK(received(*nic, &got) == r1024 && got);
  CHECK(received(*nic, &got) == r60 && got);
  CHECK(!nic->receive(ByteView(), nullptr));
  CHECK(nic->statistics().frames_received == 3 && nic->statistics().frames_dropped == 2);
  // A buffer too small takes what fits; the length says how long the frame was.
  host.deliver(0x81, r1514, 512);
  uint8_t small[100];
  uint32_t length = 0;
  CHECK(nic->receive(ByteView(small), &length) && length == 1514 && memcmp(small, r1514.data(), 100) == 0);

  CHECK(nic->accept_all_multicast(true) && host.packet_filter == 0x0E);
  CHECK(!nic->accept_all_multicast(false) && host.packet_filter == 0x0C);

  // Unplugged: nothing works any more.
  host.generation++;
  CHECK(!nic->link_up() && !nic->transmit(view(f100)));
  host.deliver(0x81, r60, 512);
  CHECK(received(*nic, &got).empty() && !got);
  nic->shutdown();
}

static void test_ecm_variants() {
  TestAllocator a;
  {  // no notification endpoint: up from the start
    FakeNicHost host;
    host.configurations.push_back(cdc_configuration(1, 0x06, 64, false));
    UniquePointer<INetworkInterface> nic = open_usb_network_interface(host, 0, a);
    CHECK(nic && nic->link_up());
    if (nic) {
      const Bytes f128 = frame(128, 9);                        // whole packets on a full-speed endpoint
      CHECK(nic->transmit(view(f128)) && host.sent.back().size() == 129);
      nic->shutdown();
      CHECK(host.data_alternate == 0);
    }
  }
  {  // a controller that cannot stream bulk IN is refused
    FakeNicHost host;
    host.stream_supported = false;
    host.configurations.push_back(cdc_configuration(1, 0x06, 512));
    CHECK(!open_usb_network_interface(host, 0, a));
  }
  {  // a bad MAC address string
    for (const char* bad : {"52540012345", "5254001234G6", "FFFFFFFFFFFF", "000000000000", "535400123456"}) {
      FakeNicHost host;
      host.configurations.push_back(cdc_configuration(1, 0x06, 512));
      host.mac = mac_string(bad);
      CHECK(!open_usb_network_interface(host, 0, a));
    }
  }
  {  // not a network device at all
    FakeNicHost host;
    host.configurations.push_back(storage_configuration(1));
    CHECK(!open_usb_network_interface(host, 0, a));
  }
}

// ---- RTL8153-like: vendor configuration 1, ECM in configuration 2 ---------------------------------------------------------------------------

static void test_second_configuration() {
  TestAllocator a;
  {
    FakeNicHost host;
    host.vendor = 0x0BDA; host.product = 0x8153;
    host.configurations.push_back(vendor_configuration(1, 512));
    host.configurations.push_back(cdc_configuration(2, 0x06, 512));
    UniquePointer<INetworkInterface> nic = open_usb_network_interface(host, 0, a);
    CHECK(nic && host.set_configuration_value == 2 && host.data_alternate == 1);
    if (nic) {
      const Bytes f = frame(200, 10);
      host.deliver(0x81, f, 512);
      bool got = false;
      CHECK(received(*nic, &got) == f && got);
      nic->shutdown();
    }
  }
  {  // a device whose current configuration is of use to another driver stays where it is
    FakeNicHost host;
    host.configurations.push_back(storage_configuration(1));
    host.configurations.push_back(cdc_configuration(2, 0x06, 512));
    CHECK(!open_usb_network_interface(host, 0, a) && host.set_configuration_value == -1);
  }
}

// ---- CDC-NCM --------------------------------------------------------------------------------------------------------------------------------

// An NTB16 the way a device builds one: the datagrams, then one NDP pointing at them.
static Bytes device_ntb(const std::vector<Bytes>& datagrams, std::vector<std::pair<uint16_t, uint16_t>> extra_entries = {}) {
  Bytes b;
  put32(b, kNcmNth16Signature); put16(b, 12); put16(b, 7); put16(b, 0); put16(b, 0);
  std::vector<std::pair<uint16_t, uint16_t>> entries;
  for (const Bytes& d : datagrams) {
    while (b.size() % 4) b.push_back(0);
    entries.push_back({static_cast<uint16_t>(b.size()), static_cast<uint16_t>(d.size())});
    b.insert(b.end(), d.begin(), d.end());
  }
  for (auto e : extra_entries) entries.push_back(e);
  while (b.size() % 4) b.push_back(0);
  const uint16_t ndp = static_cast<uint16_t>(b.size());
  put32(b, kNcmNdp16Signature); put16(b, static_cast<uint16_t>(8 + 4 * (entries.size() + 1))); put16(b, 0);
  for (auto e : entries) { put16(b, e.first); put16(b, e.second); }
  put16(b, 0); put16(b, 0);
  b[8] = static_cast<uint8_t>(b.size()); b[9] = static_cast<uint8_t>(b.size() >> 8);
  b[10] = static_cast<uint8_t>(ndp); b[11] = static_cast<uint8_t>(ndp >> 8);
  return b;
}

static void test_ncm() {
  TestAllocator a;
  FakeNicHost host;
  host.configurations.push_back(cdc_configuration(1, 0x0D, 512));
  UniquePointer<INetworkInterface> nic = open_usb_network_interface(host, 0, a);
  CHECK(nic);
  if (!nic) return;
  CHECK(strcmp(nic->name(), "USB Ethernet (CDC-NCM)") == 0);
  CHECK(host.ntb_input_size_set && host.ntb_input_size == CdcEthernetInterface::kNtbBuffer && host.data_alternate == 1);

  // Transmit: one datagram in an NTB, placed at offset % 4 == 2 as the device asked.
  const Bytes f = frame(300, 11);
  CHECK(nic->transmit(view(f)) && nic->transmit(view(f)));
  CHECK(host.sent.size() == 2);
  const Bytes& ntb = host.sent[0];
  CHECK(get32(ntb, 0) == kNcmNth16Signature && get16(ntb, 4) == 12 && get16(ntb, 8) == ntb.size() && get16(ntb, 10) == 12);
  CHECK(get32(ntb, 12) == kNcmNdp16Signature && get16(ntb, 16) == 16 && get16(ntb, 18) == 0);
  const uint16_t at = get16(ntb, 20), length = get16(ntb, 22);
  CHECK(at % 4 == 2 && length == 300 && get16(ntb, 24) == 0 && get16(ntb, 26) == 0);
  CHECK(at + length <= ntb.size() && Bytes(ntb.begin() + at, ntb.begin() + at + length) == f);
  CHECK(get16(host.sent[1], 6) == get16(ntb, 6) + 1);          // the sequence counts

  // Receive: three datagrams in one block, then a block with an entry pointing into the header and one overlapping another.
  const Bytes d1 = frame(60, 12), d2 = frame(1514, 13), d3 = frame(99, 14);
  host.deliver(0x81, device_ntb({d1, d2, d3}), 512);
  bool got = false;
  CHECK(received(*nic, &got) == d1 && got);
  CHECK(received(*nic, &got) == d2 && got);
  CHECK(received(*nic, &got) == d3 && got);
  CHECK(received(*nic, &got).empty() && !got);
  host.deliver(0x81, device_ntb({d1}, {{4, 60}, {12, 40}}), 512);
  CHECK(received(*nic, &got) == d1 && got);
  CHECK(received(*nic, &got).empty() && !got);
  CHECK(nic->statistics().frames_received == 4 && nic->statistics().frames_dropped == 2);
  // Not an NTB at all.
  host.deliver(0x81, frame(200, 15), 512);
  CHECK(received(*nic, &got).empty() && !got && nic->statistics().frames_dropped == 3);
  nic->shutdown();
}

// ---- AX88179 --------------------------------------------------------------------------------------------------------------------------------

static Bytes ax_aggregate(const std::vector<std::pair<Bytes, uint32_t>>& frames) {   // frame, extra header bits
  Bytes b, headers;
  for (auto& f : frames) {
    const uint32_t length = static_cast<uint32_t>(f.first.size() + 2);
    b.push_back(0); b.push_back(0);                            // the IP alignment pad
    b.insert(b.end(), f.first.begin(), f.first.end());
    while (b.size() % 8) b.push_back(0);
    put32(headers, (length << 16) | f.second);
  }
  const uint32_t table = static_cast<uint32_t>(b.size());
  b.insert(b.end(), headers.begin(), headers.end());
  put32(b, static_cast<uint32_t>(frames.size()) | (table << 16));
  return b;
}

static void test_ax88179() {
  TestAllocator a;
  FakeNicHost host;
  host.device = FakeNicHost::Kind::Ax88179;
  host.vendor = 0x0B95; host.product = 0x1790;
  host.configurations.push_back(vendor_configuration(1, 1024));
  host.mac_registers[0x10] = Bytes{0x00, 0x0E, 0xC6, 0x88, 0x79, 0x01};
  host.mac_registers[0x02] = Bytes{0x04};                       // on a SuperSpeed port
  host.phy[0x11] = 0x0400 | 0x8000 | 0x2000;                    // link, gigabit, full duplex
  host.phy[0x00] = 0x1140;
  UniquePointer<INetworkInterface> nic = open_usb_network_interface(host, 0, a);
  CHECK(nic);
  if (!nic) return;
  const MacAddress want{{0x00, 0x0E, 0xC6, 0x88, 0x79, 0x01}};
  CHECK(nic->mac_address() == want && host.set_configuration_value == 1);
  CHECK(host.mac_registers[0x0B] == (Bytes{0xA8, 0x03}));      // DROPCRCERR | IPE | START | AP | AB
  CHECK(host.mac_registers[0x26] == (Bytes{0x20, 0x00}) && host.mac_registers[0x33] == (Bytes{0x03}));
  CHECK(host.mac_registers[0x34] == (Bytes{0}) && host.mac_registers[0x35] == (Bytes{0}));
  CHECK(nic->link_up() && nic->link_speed_mbps() == 1000 && nic->link_duplex() == LinkDuplex::Full);
  CHECK(host.mac_registers[0x22] == (Bytes{0x3B, 0x01}));      // RECEIVE_EN | TX/RX flow | 125 MHz | full | giga
  CHECK(host.mac_registers[0x2E] == (Bytes{7, 0x4F, 0, 0x12, 0xFF}));

  // The link goes down (the status packet says so, the PHY agrees), then comes back at 100 Mb/s half duplex.
  host.phy[0x11] = 0;
  host.in[0x81].push_back(Bytes{0, 0, 0, 0, 0, 0, 0, 0});
  CHECK(!nic->link_up() && nic->link_speed_mbps() == 0);
  host.phy[0x11] = 0x0400 | 0x4000;
  host.in[0x81].push_back(Bytes{0, 0, 1, 0, 0, 0, 0, 0});
  CHECK(nic->link_up() && nic->link_speed_mbps() == 100 && nic->link_duplex() == LinkDuplex::Half);
  CHECK(host.mac_registers[0x22] == (Bytes{0x30, 0x03}) && host.mac_registers[0x2E] == (Bytes{7, 0xAE, 7, 0x18, 0xFF}));

  // Transmit: the 8-byte header, and the padding flag plus a byte when header and frame are whole packets.
  const Bytes f100 = frame(100, 20), f1016 = frame(1016, 21);
  CHECK(nic->transmit(view(f100)) && nic->transmit(view(f1016)));
  CHECK(host.sent.size() == 2 && host.sent[0].size() == 108 && get32(host.sent[0], 0) == 100 && get32(host.sent[0], 4) == 0);
  CHECK(Bytes(host.sent[0].begin() + 8, host.sent[0].end()) == f100);
  CHECK(host.sent[1].size() == 1025 && get32(host.sent[1], 0) == 1016 && get32(host.sent[1], 4) == 0x80008000u && host.sent[1][1024] == 0);

  // Receive: an aggregate of three frames, the middle one with a CRC error.
  const Bytes r1 = frame(60, 22), r2 = frame(500, 23), r3 = frame(1514, 24);
  host.deliver(0x82, ax_aggregate({{r1, 0}, {r2, Ax88179RxParser::kHeaderCrcError}, {r3, 0}}), 1024);
  bool got = false;
  CHECK(received(*nic, &got) == r1 && got);
  CHECK(received(*nic, &got) == r3 && got);
  CHECK(received(*nic, &got).empty() && !got);
  CHECK(nic->statistics().frames_received == 2 && nic->statistics().frames_dropped == 1);

  CHECK(nic->accept_all_multicast(true) && host.mac_registers[0x0B] == (Bytes{0xAA, 0x03}));
  CHECK(!nic->accept_all_multicast(false) && host.mac_registers[0x0B] == (Bytes{0xA8, 0x03}));
  nic->shutdown();
  CHECK(host.mac_registers[0x0B] == (Bytes{0, 0}));

  // A device that answers with no address is not taken.
  FakeNicHost blank;
  blank.device = FakeNicHost::Kind::Ax88179;
  blank.vendor = 0x0B95; blank.product = 0x178A;
  blank.configurations.push_back(vendor_configuration(1, 512));
  CHECK(!open_usb_network_interface(blank, 0, a));
  CHECK(Ax88179NetworkInterface::supports(0x2001, 0x4A00) && !Ax88179NetworkInterface::supports(0x0B95, 0x7720));
}

// ---- the parsers on their own ---------------------------------------------------------------------------------------------------------------

static bool inside(ConstByteView part, const Bytes& whole) { return part.data() >= whole.data() && part.data() + part.size() <= whole.data() + whole.size(); }

static void test_ncm_parser() {
  const Bytes good = device_ntb({frame(60, 1), frame(1514, 2), frame(14, 3)});
  NcmNtbParser p;
  ConstByteView d;
  CHECK(p.start(view(good)));
  int n = 0;
  while (p.next(&d)) ++n;
  CHECK(n == 3 && p.malformed() == 0);

  // Truncated: the block length no longer fits.
  for (size_t cut = 0; cut < good.size(); ++cut) {
    Bytes t(good.begin(), good.begin() + cut);
    CHECK(!p.start(view(t)));
    while (p.next(&d)) CHECK(false);
  }
  // Every bit flipped: whatever comes out lies inside the block and does not overlap what came before.
  for (size_t bit = 0; bit < good.size() * 8; ++bit) {
    Bytes f = good;
    f[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
    if (!p.start(view(f))) continue;
    std::vector<ConstByteView> out;
    while (p.next(&d)) {
      CHECK(inside(d, f) && d.size() >= 14 && d.data() >= f.data() + 12);
      for (ConstByteView o : out) CHECK(d.data() + d.size() <= o.data() || o.data() + o.size() <= d.data());
      out.push_back(d);
    }
  }
  // A table chain that loops back on itself ends.
  Bytes loop = good;
  const uint16_t ndp = get16(loop, 10);
  loop[ndp + 6] = static_cast<uint8_t>(ndp); loop[ndp + 7] = static_cast<uint8_t>(ndp >> 8);
  CHECK(p.start(view(loop)));
  n = 0;
  while (p.next(&d)) ++n;
  CHECK(n == 3 && p.malformed() == 1);
  // Overlapping datagrams: the second is refused.
  const Bytes overlap = device_ntb({frame(100, 4)}, {{40, 60}});
  CHECK(p.start(view(overlap)));
  n = 0;
  while (p.next(&d)) ++n;
  CHECK(n == 1 && p.malformed() == 1);

  // The builder and the parser agree; a buffer too small is refused.
  uint8_t out[2048];
  const Bytes f = frame(1514, 5);
  const uint32_t length = build_ncm_ntb16(ByteView(out), view(f), 3, 8, 3, 0);
  CHECK(length > 0 && p.start(ConstByteView(out, length)) && p.next(&d) && d.size() == 1514 && memcmp(d.data(), f.data(), 1514) == 0 && (d.data() - out) % 8 == 3);
  CHECK(build_ncm_ntb16(ByteView(out).first(1500), view(f), 0, 4, 0, 0) == 0 && build_ncm_ntb16(ByteView(out), view(f), 0, 4, 0, 1000) == 0);
  CHECK(build_ncm_ntb16(ByteView(out), ConstByteView(), 0, 4, 0, 0) == 0);

  NcmNtbParameters params;
  Bytes pb;
  put16(pb, 28); put16(pb, 1); put32(pb, 8192); put16(pb, 4); put16(pb, 0); put16(pb, 4); put16(pb, 0); put32(pb, 8192); put16(pb, 4); put16(pb, 0); put16(pb, 4); put16(pb, 0);
  CHECK(parse_ncm_ntb_parameters(view(pb), &params) && params.in_max_size == 8192);
  for (size_t cut = 0; cut < pb.size(); ++cut) CHECK(!parse_ncm_ntb_parameters(ConstByteView(pb.data(), cut), &params));
}

static void test_ax_parser() {
  const Bytes good = ax_aggregate({{frame(60, 1), 0}, {frame(1514, 2), 0}, {frame(100, 3), Ax88179RxParser::kHeaderDropError}});
  Ax88179RxParser p;
  ConstByteView d;
  CHECK(p.start(view(good)));
  int n = 0;
  while (p.next(&d)) ++n;
  CHECK(n == 2 && p.errors() == 1);
  for (size_t cut = 0; cut < good.size(); ++cut) {
    Bytes t(good.begin(), good.begin() + cut);
    if (!p.start(view(t))) continue;
    while (p.next(&d)) CHECK(inside(d, t));
  }
  for (size_t bit = 0; bit < good.size() * 8; ++bit) {
    Bytes f = good;
    f[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
    if (!p.start(view(f))) continue;
    const uint32_t table = get32(f, f.size() - 4) >> 16;
    while (p.next(&d)) CHECK(inside(d, f) && d.data() + d.size() <= f.data() + table && d.size() >= 14);
  }
  // A table outside the transfer, and a frame that runs into the table.
  Bytes outside = good;
  outside[outside.size() - 1] = 0x7F;
  CHECK(!p.start(view(outside)));
  Bytes into = ax_aggregate({{frame(60, 1), 0}});
  into[into.size() - 8 + 2] = 0xFF;                             // the length of the first frame: 0x1FFF
  into[into.size() - 8 + 3] = 0x1F;
  CHECK(p.start(view(into)) && !p.next(&d) && p.errors() == 1);
  uint8_t out[64];
  CHECK(ax88179_build_tx(ByteView(out), view(frame(60, 0)), 512) == 0);   // does not fit
}

static void test_descriptors() {
  const Bytes ecm = cdc_configuration(1, 0x06, 512);
  UsbCdcEthernetFunction f;
  CHECK(find_cdc_ethernet_function(view(ecm), &f) && f.protocol == UsbNetworkProtocol::Ecm && f.configuration == 1 && f.control_interface == 0 &&
        f.data_interface == 1 && f.data_alternate == 1 && f.mac_string == 3 && f.max_segment == 1514 && f.notify_endpoint == 0x83 &&
        f.bulk_in == 0x81 && f.bulk_out == 0x02 && f.in_max_packet == 512);
  CHECK(!find_cdc_ethernet_function(view(ecm), UsbNetworkProtocol::Ncm, &f));
  const Bytes vendor = vendor_configuration(3, 1024);
  UsbVendorNetworkEndpoints e;
  CHECK(find_vendor_network_endpoints(view(vendor), &e) && e.configuration == 3 && e.bulk_in == 0x82 && e.bulk_out == 0x03 && e.interrupt_in == 0x81 && e.in_max_packet == 1024);
  CHECK(usb_configuration_without_driver(view(vendor)) && !usb_configuration_without_driver(view(ecm)));
  // RNDIS (QEMU's usb-net and phones put it first): control 02/02/FF + CDC data. A modem (ACM, 02/02/01) with the same data interface has a driver.
  const auto two_interfaces = [](uint8_t c, uint8_t s, uint8_t p) {
    return Bytes{9, 2, 27, 0, 2, 1, 0, 0x80, 50, 9, 4, 0, 0, 0, c, s, p, 0, 9, 4, 1, 0, 0, 0x0A, 0, 0, 0};
  };
  CHECK(usb_configuration_without_driver(view(two_interfaces(0x02, 0x02, 0xFF))) && usb_configuration_without_driver(view(two_interfaces(0xE0, 0x01, 0x03))));
  CHECK(!usb_configuration_without_driver(view(two_interfaces(0x02, 0x02, 0x01))) && !usb_configuration_without_driver(view(two_interfaces(0x08, 0x06, 0x50))));
  for (const Bytes* blob : {&ecm, &vendor}) {
    for (size_t cut = 0; cut < blob->size(); ++cut) {
      Bytes t(blob->begin(), blob->begin() + cut);
      if (find_cdc_ethernet_function(view(t), &f)) CHECK(f.valid());
      if (find_vendor_network_endpoints(view(t), &e)) CHECK(e.valid());
    }
    for (size_t bit = 0; bit < blob->size() * 8; ++bit) {
      Bytes t = *blob;
      t[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
      if (find_cdc_ethernet_function(view(t), &f)) CHECK(f.valid());
      if (find_vendor_network_endpoints(view(t), &e)) CHECK(e.valid());
      usb_configuration_without_driver(view(t));
    }
  }
  MacAddress mac;
  CHECK(parse_usb_mac_string(view(mac_string("0a1B2c3D4e5F")), &mac) && mac.bytes[0] == 0x0A && mac.bytes[5] == 0x5F);
  const Bytes s = mac_string("525400123456");
  for (size_t cut = 0; cut < s.size(); ++cut) CHECK(!parse_usb_mac_string(ConstByteView(s.data(), cut), &mac));

  // The assembler: a transfer too big for it is dropped whole, the next one comes through.
  uint8_t storage[100];
  UsbTransferAssembler as{ByteView(storage)};
  const Bytes piece = frame(64, 0), tail = frame(10, 1);
  CHECK(!as.push(view(piece), 64) && !as.push(view(piece), 64) && !as.push(view(tail), 64) && as.overflows() == 1);
  CHECK(!as.push(view(piece), 64) && as.push(view(tail), 64) && as.data().size() == 74);
  as.release();
  CHECK(!as.push(ConstByteView(), 64));                         // a lone zero-length packet is no transfer
}

int main() {
  test_descriptors();
  test_ncm_parser();
  test_ax_parser();
  test_ecm();
  test_ecm_variants();
  test_second_configuration();
  test_ncm();
  test_ax88179();
  if (g_failed) {
    printf("UsbNetworkTest: %d checks FAILED\n", g_failed);
    return 1;
  }
  printf("UsbNetworkTest: all %d checks passed\n", g_checks);
  return 0;
}
