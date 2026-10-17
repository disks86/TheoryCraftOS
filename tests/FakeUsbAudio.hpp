// Shared by the USB audio tests: a counting allocator, USB audio class configuration descriptors (UAC1 and UAC2) and a fake host controller
// with an audio device behind it that records the isochronous packets it is given.
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <vector>
#include "Hardware/AudioDevice/SystemAudioDevices.hpp"
#include "Hardware/AudioDevice/Usb/UsbAudioDescriptors.hpp"
#include "Hardware/AudioDevice/Usb/UsbAudioDevice.hpp"

using namespace hardware;

struct TestAllocator final : allocators::IAllocator {
  long live = 0;   // (atomic: threads share the allocator in the thread tests)
  void* allocate(uint64_t size) override { if (!size) return nullptr; __atomic_add_fetch(&live, 1, __ATOMIC_RELAXED); return aligned_alloc(64, (size + 63) / 64 * 64); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    if (!size) return nullptr;
    __atomic_add_fetch(&live, 1, __ATOMIC_RELAXED);
    if (alignment < 64) alignment = 64;
    return aligned_alloc(alignment, (size + alignment - 1) / alignment * alignment);
  }
  void deallocate(void* p) override { if (p) { __atomic_sub_fetch(&live, 1, __ATOMIC_RELAXED); free(p); } }
};

struct FakeClock final : IClock {
  const char* name() const override { return "fake clock"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint64_t now_us() const override { return 0; }
};

struct FakePci final : IPciBus {
  const char* name() const override { return "fake pci"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint32_t device_count() const override { return 0; }
  bool pci_device(uint32_t, PciDeviceInfo*) const override { return false; }
  uint32_t read32(PciAddress, uint16_t) const override { return 0; }
  void write32(PciAddress, uint16_t, uint32_t) const override {}
  uint64_t memory_bar(PciAddress, uint8_t) const override { return 0; }
  uint16_t find_capability(PciAddress, uint8_t) const override { return 0; }
};

typedef std::vector<uint8_t> Bytes;
static void put(Bytes& v, std::initializer_list<uint8_t> b) { v.insert(v.end(), b.begin(), b.end()); }

// ------------------------------------------------------------------------------------------------ configuration builders

struct Layout {
  int version = 1;               // 1 or 2
  uint8_t channels = 2, subslot = 2, bits = 16;
  uint32_t rate = 48000;         // what the device offers (UAC1: in the format; UAC2: what the clock answers)
  bool feature_unit = true;
  bool volume = true, mute = true;
  uint8_t sync_attr = 0x09;      // endpoint bmAttributes: 0x05 asynchronous, 0x09 adaptive, 0x0D synchronous
  uint16_t max_packet = 192;
  uint8_t interval = 1;          // bInterval
  bool feedback = false;
  bool output = true, input = false;
  uint8_t in_channels = 1;
  uint16_t in_max_packet = 96;
};

static Bytes build_config(const Layout& l) {
  Bytes c, ac, as;
  const bool v2 = l.version == 2;
  const uint8_t ifaces = static_cast<uint8_t>(1 + (l.output ? 1 : 0) + (l.input ? 1 : 0));
  put(c, {9, 2, 0, 0, ifaces, 1, 0, 0x80, 50});
  // control interface
  put(c, {9, 4, 0, 0, 0, 1, 1, static_cast<uint8_t>(v2 ? 0x20 : 0), 0});
  Bytes body;
  const uint8_t clock = 0x10;
  if (v2) put(body, {8, 0x24, 0x0A, clock, 0x03, 0x03, 0, 0});
  if (l.output) {
    if (v2) put(body, {17, 0x24, 2, 1, 1, 1, 0, clock, 2, 3, 0, 0, 0, 0, 0, 0, 0});
    else put(body, {12, 0x24, 2, 1, 1, 1, 0, 2, 3, 0, 0, 0});
    uint8_t source = 1;
    if (l.feature_unit) {
      const uint8_t ctl = static_cast<uint8_t>((l.mute ? 1 : 0) | (l.volume ? 2 : 0));
      if (v2) put(body, {18, 0x24, 6, 2, 1, static_cast<uint8_t>((l.mute ? 3 : 0) | (l.volume ? 0xC : 0)), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
      else put(body, {10, 0x24, 6, 2, 1, 1, ctl, 0, 0, 0});
      source = 2;
    }
    if (v2) put(body, {12, 0x24, 3, 3, 1, 3, 0, source, clock, 0, 0, 0});
    else put(body, {9, 0x24, 3, 3, 1, 3, 0, source, 0});
  }
  if (l.input) {
    if (v2) put(body, {17, 0x24, 2, 4, 1, 2, 0, clock, 1, 0, 0, 0, 0, 0, 0, 0, 0});
    else put(body, {12, 0x24, 2, 4, 1, 2, 0, 1, 0, 0, 0, 0});
    if (v2) put(body, {12, 0x24, 3, 5, 1, 1, 0, 4, clock, 0, 0, 0});
    else put(body, {9, 0x24, 3, 5, 1, 1, 0, 4, 0});
  }
  const uint16_t total = static_cast<uint16_t>((v2 ? 9 : 8 + (ifaces - 1)) + body.size());
  if (v2) put(c, {9, 0x24, 1, 0, 2, 1, static_cast<uint8_t>(total), static_cast<uint8_t>(total >> 8), 0});
  else {
    put(c, {static_cast<uint8_t>(8 + ifaces - 1), 0x24, 1, 0, 1, static_cast<uint8_t>(total), static_cast<uint8_t>(total >> 8), static_cast<uint8_t>(ifaces - 1)});
    for (uint8_t i = 1; i < ifaces; ++i) c.push_back(i);
  }
  c.insert(c.end(), body.begin(), body.end());

  auto stream = [&](uint8_t iface, uint8_t link, bool in, uint8_t channels, uint16_t max_packet, bool feedback) {
    put(c, {9, 4, iface, 0, 0, 1, 2, static_cast<uint8_t>(v2 ? 0x20 : 0), 0});
    put(c, {9, 4, iface, 1, static_cast<uint8_t>(feedback ? 2 : 1), 1, 2, static_cast<uint8_t>(v2 ? 0x20 : 0), 0});
    if (v2) {
      put(c, {16, 0x24, 1, link, 0, 1, 1, 0, 0, 0, channels, 3, 0, 0, 0, 0});
      put(c, {6, 0x24, 2, 1, l.subslot, l.bits});
    } else {
      put(c, {7, 0x24, 1, link, 1, 1, 0});
      put(c, {11, 0x24, 2, 1, channels, l.subslot, l.bits, 1, static_cast<uint8_t>(l.rate), static_cast<uint8_t>(l.rate >> 8), static_cast<uint8_t>(l.rate >> 16)});
    }
    const uint8_t attr = in ? 0x0D : l.sync_attr;
    put(c, {9, 5, static_cast<uint8_t>(in ? 0x81 : 0x01), attr, static_cast<uint8_t>(max_packet), static_cast<uint8_t>(max_packet >> 8), l.interval, 0, 0});
    if (v2) put(c, {8, 0x25, 1, 0, 0, 0, 0, 0});
    else put(c, {7, 0x25, 1, 1, 0, 0, 0});
    if (feedback) put(c, {9, 5, 0x82, 0x11, 4, 0, l.interval, 3, 0});
  };
  uint8_t iface = 1;
  if (l.output) stream(iface++, 1, false, l.channels, l.max_packet, l.feedback);
  if (l.input) {
    // the microphone's endpoint is 0x81 in stream() but shares the number space with the feedback endpoint 0x82: they are different addresses
    stream(iface++, 5, true, l.in_channels, l.in_max_packet, false);
  }
  c[2] = static_cast<uint8_t>(c.size());
  c[3] = static_cast<uint8_t>(c.size() >> 8);
  return c;
}

// ------------------------------------------------------------------------------------------------ the fake host

struct FakeAudioHost final : IUsbHostController {
  struct Control { uint8_t type, request; uint16_t value, index; Bytes data; };
  struct Endpoint {
    uint32_t interval_us = 0, max_packet = 0, pending = 0;
    std::vector<Bytes> sent;       // OUT: every packet submitted
    std::vector<Bytes> incoming;   // IN: packets the device "sent"
    size_t taken = 0;
    uint64_t completed = 0;
    bool running = false;
  };
  Bytes config;
  UsbSpeed speed = UsbSpeed::Full;
  bool isochronous = true;
  uint16_t vendor = 0x1234, product = 0x5678;
  std::vector<Control> controls;
  std::map<uint8_t, Endpoint> endpoints;
  std::vector<uint8_t> started, stopped;
  int set_configurations = 0;
  // what GET_MIN / GET_MAX / RANGE of the volume say
  int16_t vol_min = -0x5000, vol_max = 0;
  uint32_t clock_rate = 48000;       // UAC2 clock source current rate
  Bytes clock_range;                 // UAC2 range answer; empty: derived from clock_rate
  bool refuse_iso = false;

  explicit FakeAudioHost(const Layout& l) : config(build_config(l)) {
    speed = l.version == 2 ? UsbSpeed::High : UsbSpeed::Full;
  }

  const char* name() const override { return "fake host"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  UsbControllerKind kind() const override { return UsbControllerKind::Xhci; }
  uint32_t port_count() const override { return 1; }
  bool port_info(uint32_t, UsbPortInfo* out) const override { out->connected = true; out->speed = speed; return true; }
  bool device_descriptor(uint32_t, UsbDeviceDescriptor* out) const override {
    memset(out, 0, sizeof *out);
    out->length = 18; out->descriptor_type = 1; out->vendor_id = vendor; out->product_id = product; out->configuration_count = 1;
    return true;
  }
  bool configuration_descriptor(uint32_t, ByteView buffer, uint16_t* length) const override {
    const size_t n = config.size() < buffer.size() ? config.size() : buffer.size();
    memcpy(buffer.data(), config.data(), n);
    *length = static_cast<uint16_t>(config.size());
    return true;
  }
  bool set_configuration(uint32_t, uint8_t) override { ++set_configurations; return true; }
  bool control_out(uint32_t, uint8_t type, uint8_t request, uint16_t value, uint16_t index) override {
    controls.push_back({type, request, value, index, {}});
    return true;
  }
  bool control_out_data(uint32_t, uint8_t type, uint8_t request, uint16_t value, uint16_t index, ConstByteView data) override {
    controls.push_back({type, request, value, index, Bytes(data.data(), data.data() + data.size())});
    return true;
  }
  bool control_in(uint32_t, uint8_t type, uint8_t request, uint16_t value, uint16_t index, ByteView buffer) override {
    memset(buffer.data(), 0, buffer.size());
    controls.push_back({type, request, value, index, {}});
    const uint8_t selector = value >> 8;
    if (type != 0xA1) return false;
    if ((index >> 8) == 0x10 && selector == 1) {   // UAC2 clock source: sampling frequency
      if (request == 0x01) { memcpy(buffer.data(), &clock_rate, 4); return true; }
      if (request == 0x02) {
        Bytes r = clock_range;
        if (r.empty()) { r = {1, 0}; for (int s : {0, 8, 16, 24}) r.push_back(static_cast<uint8_t>(clock_rate >> s)); for (int s : {0, 8, 16, 24}) r.push_back(static_cast<uint8_t>(clock_rate >> s)); r.insert(r.end(), {0, 0, 0, 0}); }
        memcpy(buffer.data(), r.data(), r.size() < buffer.size() ? r.size() : buffer.size());
        return true;
      }
    }
    if (selector == 2 && (index >> 8) == 2) {   // feature unit volume
      if (request == 0x82) { memcpy(buffer.data(), &vol_min, 2); return true; }
      if (request == 0x83) { memcpy(buffer.data(), &vol_max, 2); return true; }
      if (request == 0x02) {
        const int16_t r[4] = {1, vol_min, vol_max, 256};
        memcpy(buffer.data(), r, buffer.size() < 8 ? buffer.size() : 8);
        return true;
      }
    }
    return false;
  }
  bool start_interrupt_in(uint32_t, uint8_t, uint16_t, uint8_t) override { return false; }
  bool poll_interrupt_in(uint32_t, uint8_t, ByteView, uint16_t*) override { return false; }
  bool start_bulk_endpoint(uint32_t, uint8_t, uint16_t) override { return false; }
  bool bulk_transfer(uint32_t, uint8_t, ByteView, uint32_t*) override { return false; }
  bool reset_bulk_endpoint(uint32_t, uint8_t) override { return false; }

  bool supports_isochronous() const override { return isochronous; }
  bool start_iso_endpoint(uint32_t, uint8_t address, uint16_t max_packet, uint8_t interval) override {
    if (refuse_iso || interval == 0 || interval > 16) return false;
    Endpoint& e = endpoints[address];
    e = Endpoint();
    e.interval_us = (speed == UsbSpeed::High ? 125u : 1000u) << (interval - 1);
    e.max_packet = max_packet;
    e.running = true;
    started.push_back(address);
    return true;
  }
  void stop_iso_endpoint(uint32_t, uint8_t address) override {
    endpoints[address].running = false;
    stopped.push_back(address);
  }
  uint32_t iso_interval_us(uint32_t, uint8_t address) override { return endpoints[address].running ? endpoints[address].interval_us : 0; }
  uint32_t iso_free_packets(uint32_t, uint8_t address) override { Endpoint& e = endpoints[address]; return e.running ? 128 - e.pending : 0; }
  bool iso_submit(uint32_t, uint8_t address, ConstByteView packet) override {
    Endpoint& e = endpoints[address];
    if (!e.running || e.pending >= 128 || packet.size() > e.max_packet) return false;
    e.sent.push_back(Bytes(packet.data(), packet.data() + packet.size()));
    ++e.pending;
    return true;
  }
  uint64_t iso_completed_packets(uint32_t, uint8_t address) override { return endpoints[address].completed; }
  bool iso_receive(uint32_t, uint8_t address, ByteView out, uint16_t* length) override {
    Endpoint& e = endpoints[address];
    if (e.taken >= e.incoming.size()) return false;
    const Bytes& p = e.incoming[e.taken++];
    const size_t n = p.size() < out.size() ? p.size() : out.size();
    if (n) memcpy(out.data(), p.data(), n);
    *length = static_cast<uint16_t>(n);
    return true;
  }

  // Time passing: the controller has sent everything queued.
  void play_out(uint8_t address) {
    Endpoint& e = endpoints[address];
    e.completed += e.pending;
    e.pending = 0;
  }
  size_t count(uint8_t type, uint8_t request, uint16_t value, uint16_t index) const {
    size_t n = 0;
    for (const Control& c : controls) n += c.type == type && c.request == request && c.value == value && c.index == index;
    return n;
  }
  const Control* find(uint8_t type, uint8_t request, uint16_t value, uint16_t index) const {
    for (const Control& c : controls)
      if (c.type == type && c.request == request && c.value == value && c.index == index) return &c;
    return nullptr;
  }
};

struct FakeUsb final : IUsbBus {
  std::vector<IUsbHostController*> hosts;
  const char* name() const override { return "fake usb"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint32_t device_count() const override { return static_cast<uint32_t>(hosts.size()); }
  uint32_t controller_count() const override { return static_cast<uint32_t>(hosts.size()); }
  UsbControllerKind controller_kind(uint32_t) const override { return UsbControllerKind::Xhci; }
  uint32_t port_count(uint32_t c) const override { return hosts[c]->port_count(); }
  bool port_info(uint32_t c, uint32_t p, UsbPortInfo* out) const override { return hosts[c]->port_info(p, out); }
  bool device_descriptor(uint32_t c, uint32_t p, UsbDeviceDescriptor* out) const override { return hosts[c]->device_descriptor(p, out); }
  IUsbHostController* host_controller(uint32_t c) override { return c < hosts.size() ? hosts[c] : nullptr; }
};

