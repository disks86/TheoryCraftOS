// Host-side tests for USB audio class support: the descriptor reader (USB Audio 1.0 and 2.0, with truncated and corrupted descriptors),
// and UsbAudioDevice against a fake host controller with a fake device behind it: bring-up requests, the isochronous packets that
// come out (size, sample format, silence, volume), feedback, capture, shutdown, and SystemAudioDevices putting USB devices in order.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <vector>
#include "Hardware/AudioDevice/SystemAudioDevices.hpp"
#include "Hardware/AudioDevice/Usb/UsbAudioDescriptors.hpp"
#include "Hardware/AudioDevice/Usb/UsbAudioDevice.hpp"

using namespace hardware;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct TestAllocator final : allocators::IAllocator {
  long live = 0;
  void* allocate(uint64_t size) override { if (!size) return nullptr; ++live; return aligned_alloc(64, (size + 63) / 64 * 64); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    if (!size) return nullptr;
    ++live;
    if (alignment < 64) alignment = 64;
    return aligned_alloc(alignment, (size + alignment - 1) / alignment * alignment);
  }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
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
  std::vector<FakeAudioHost*> hosts;
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

// ------------------------------------------------------------------------------------------------ helpers

struct Rig {
  TestAllocator alloc;
  List<UniquePointer<UsbAudioDevice>> devices{alloc};
  FakeUsb usb;
  explicit Rig(FakeAudioHost& host) { usb.hosts.push_back(&host); UsbAudioDevice::discover(usb, alloc, devices); }
  UsbAudioDevice* dev() { return devices.count() ? devices[0].get() : nullptr; }
};

static Bytes ramp(uint32_t frames, int base = 1) {
  Bytes b;
  for (uint32_t i = 0; i < frames; ++i) {
    const int16_t l = static_cast<int16_t>(base + static_cast<int>(i)), r = static_cast<int16_t>(-(base + static_cast<int>(i)));
    b.push_back(static_cast<uint8_t>(l)); b.push_back(static_cast<uint8_t>(l >> 8));
    b.push_back(static_cast<uint8_t>(r)); b.push_back(static_cast<uint8_t>(r >> 8));
  }
  return b;
}

// Plays `pcm` through the device, letting the fake controller drain the queue as it goes, and returns every packet sent on `endpoint`.
static void play_all(UsbAudioDevice& d, FakeAudioHost& h, uint8_t endpoint, const Bytes& pcm) {
  size_t offset = 0;
  for (int round = 0; round < 20000 && (offset < pcm.size() || d.queued_bytes() > 0); ++round) {
    if (offset < pcm.size()) offset += d.write(ConstByteView(pcm.data() + offset, pcm.size() - offset));
    h.play_out(endpoint);
    d.queued_bytes();
  }
  h.play_out(endpoint);
  d.queued_bytes();
}

// The sent stream with the leading silence cut off.
static Bytes payload(const FakeAudioHost::Endpoint& e, size_t align = 1) {
  Bytes all;
  for (const Bytes& p : e.sent) all.insert(all.end(), p.begin(), p.end());
  size_t start = 0;
  while (start < all.size() && all[start] == 0) ++start;
  start -= start % align;
  return Bytes(all.begin() + start, all.end());
}

// ------------------------------------------------------------------------------------------------ tests

static void test_descriptors_uac1() {
  Layout l;
  l.input = true;
  const Bytes c = build_config(l);
  UacDevice m;
  CHECK(parse_uac_configuration(ConstByteView(c.data(), c.size()), &m));
  CHECK(m.version == 1 && m.configuration == 1 && m.control_interface == 0);
  CHECK(m.stream_count == 2);
  CHECK(!m.streams[0].input && m.streams[0].interface == 1 && m.streams[0].format_count == 1);
  CHECK(m.streams[1].input && m.streams[1].interface == 2);
  const UacFormat& f = m.streams[0].formats[0];
  CHECK(f.alternate == 1 && f.channels == 2 && f.subslot == 2 && f.bits == 16 && f.pcm && f.rate_48000 && f.endpoint == 1 && f.max_packet == 192);
  CHECK(f.sync == 2 && f.rate_control);
  const UacEntity* fu = uac_feature_unit(m, m.streams[0]);
  CHECK(fu && fu->id == 2 && fu->volume[0] && fu->mute[0]);
  CHECK(uac_feature_unit(m, m.streams[1]) == nullptr);
  CHECK(uac_clock_source(m, m.streams[0]) == nullptr);
  CHECK(m.streams[1].formats[0].endpoint == 0x81 && m.streams[1].formats[0].channels == 1);

  Layout other;
  other.rate = 44100;
  const Bytes c2 = build_config(other);
  CHECK(!parse_uac_configuration(ConstByteView(c2.data(), c2.size()), &m) || m.stream_count == 0 || !m.streams[0].formats[0].rate_48000);

  Bytes empty;
  CHECK(!parse_uac_configuration(ConstByteView(empty.data(), 0), &m));
}

static void test_descriptors_uac2() {
  Layout l;
  l.version = 2;
  l.interval = 4;
  l.max_packet = 192;
  const Bytes c = build_config(l);
  UacDevice m;
  CHECK(parse_uac_configuration(ConstByteView(c.data(), c.size()), &m));
  CHECK(m.version == 2 && m.stream_count == 1);
  const UacFormat& f = m.streams[0].formats[0];
  CHECK(f.channels == 2 && f.subslot == 2 && f.bits == 16 && f.rate_unknown && f.interval == 4);
  const UacEntity* clock = uac_clock_source(m, m.streams[0]);
  CHECK(clock && clock->id == 0x10 && clock->clock_programmable);
  const UacEntity* fu = uac_feature_unit(m, m.streams[0]);
  CHECK(fu && fu->volume[0] && fu->mute[0]);
}

static void test_descriptor_fuzz() {
  uint32_t seed = 99;
  auto rnd = [&] { seed = seed * 1664525 + 1013904223; return seed >> 8; };
  Layout variants[4];
  variants[0].input = true;
  variants[1].version = 2; variants[1].input = true; variants[1].interval = 4;
  variants[2].feedback = true; variants[2].sync_attr = 0x05; variants[2].max_packet = 200;
  variants[3].version = 2; variants[3].feature_unit = false;
  for (const Layout& l : variants) {
    const Bytes base = build_config(l);
    for (int round = 0; round < 3000; ++round) {
      Bytes c = base;
      switch (round % 3) {
        case 0: c.resize(rnd() % (c.size() + 1)); break;
        case 1: for (int k = 0; k < 1 + static_cast<int>(rnd() % 4); ++k) c[rnd() % c.size()] ^= static_cast<uint8_t>(1u << (rnd() % 8)); break;
        case 2: for (int k = 0; k < 1 + static_cast<int>(rnd() % 3); ++k) c[rnd() % c.size()] = static_cast<uint8_t>(rnd()); break;
      }
      std::vector<uint8_t> exact(c);   // exactly sized, so the sanitizer sees any read past the end
      UacDevice m;
      parse_uac_configuration(ConstByteView(exact.data(), exact.size()), &m);
      if (round % 50 == 0) {   // and through the whole device
        FakeAudioHost host(l);
        host.config = exact;
        Rig rig(host);
        if (rig.dev()) {
          rig.dev()->write(ConstByteView(ramp(100).data(), 400));
          rig.dev()->queued_bytes();
        }
      }
    }
  }
}

static void test_uac1_playback() {
  Layout l;
  FakeAudioHost host(l);
  Rig rig(host);
  UsbAudioDevice* d = rig.dev();
  CHECK(d != nullptr);
  if (!d) return;
  CHECK(d->has_output() && !d->has_input());
  CHECK(strcmp(d->name(), "USB audio device 1234:5678") == 0);
  CHECK(d->capture() == nullptr);
  CHECK(host.set_configurations == 1);
  // the interface is switched to alternate 1 and the rate is written to the endpoint
  CHECK(host.count(0x01, 11, 1, 1) == 1);
  const FakeAudioHost::Control* rate = host.find(0x22, 0x01, 0x0100, 1);
  CHECK(rate && rate->data == Bytes({0x80, 0xBB, 0x00}));
  CHECK(host.endpoints[1].running && host.endpoints[1].interval_us == 1000);
  // the volume range was read from the feature unit and the volume (80 %) set on the master channel
  CHECK(host.count(0xA1, 0x82, 0x0200, 0x0200) == 1 && host.count(0xA1, 0x83, 0x0200, 0x0200) == 1);
  const FakeAudioHost::Control* vol = host.find(0x21, 0x01, 0x0200, 0x0200);
  // 80 % of the top 60 dB (15360 units): 0 - 20 % * 15360 = -3072 = 0xF400
  CHECK(vol && vol->data == Bytes({0x00, 0xF4}));

  const Bytes pcm = ramp(4810);
  play_all(*d, host, 1, pcm);
  const FakeAudioHost::Endpoint& e = host.endpoints[1];
  CHECK(!e.sent.empty());
  for (const Bytes& p : e.sent) CHECK(p.size() == 192);
  const Bytes got = payload(e);
  CHECK(got.size() >= pcm.size() && memcmp(got.data(), pcm.data(), pcm.size()) == 0);   // exactly what was written, in order, then silence
  bool silent = true;
  for (size_t i = pcm.size(); i < got.size(); ++i) silent = silent && got[i] == 0;
  CHECK(silent);
  const AudioStatistics s = d->statistics();
  CHECK(s.frames_queued == 4810 && s.frames_played >= 4810);
  CHECK(s.underruns >= 1);   // it ran dry afterwards (and at the start, before anything was written)
}

static void test_ring_full_and_order() {
  Layout l;
  FakeAudioHost host(l);
  Rig rig(host);
  UsbAudioDevice* d = rig.dev();
  if (!d) { CHECK(false); return; }
  const Bytes big = ramp(20000);
  const uint32_t taken = d->write(ConstByteView(big.data(), big.size()));
  CHECK(taken > 0 && taken < big.size() && taken % 4 == 0);   // the ring (64 KiB) took what fits
  CHECK(d->free_bytes() == UsbAudioDevice::kRingBytes - taken || d->free_bytes() <= UsbAudioDevice::kRingBytes);
  // the controller never completes: at most the lead (24 packets) is queued
  d->queued_bytes();
  d->queued_bytes();
  CHECK(host.endpoints[1].pending <= 24 && host.endpoints[1].pending >= 4);
  // a packet's worth of frames at a time, from the front of the stream
  host.play_out(1);
  d->queued_bytes();
  const Bytes got = payload(host.endpoints[1]);
  CHECK(got.size() >= 192 && memcmp(got.data(), big.data(), 192) == 0);
}

static void test_mono_device() {
  Layout l;
  l.channels = 1;
  l.max_packet = 96;
  FakeAudioHost host(l);
  Rig rig(host);
  UsbAudioDevice* d = rig.dev();
  if (!d) { CHECK(false); return; }
  Bytes pcm;
  for (int i = 0; i < 480; ++i) {   // left 1000+i, right 3000+i: the mix is 2000+i
    const int16_t left = static_cast<int16_t>(1000 + i), right = static_cast<int16_t>(3000 + i);
    pcm.push_back(static_cast<uint8_t>(left)); pcm.push_back(static_cast<uint8_t>(left >> 8));
    pcm.push_back(static_cast<uint8_t>(right)); pcm.push_back(static_cast<uint8_t>(right >> 8));
  }
  play_all(*d, host, 1, pcm);
  for (const Bytes& p : host.endpoints[1].sent) CHECK(p.size() == 96);
  const Bytes got = payload(host.endpoints[1]);
  CHECK(got.size() >= 960);
  for (int i = 0; i < 480 && got.size() >= 960; ++i) {
    const int16_t v = static_cast<int16_t>(got[2 * i] | (got[2 * i + 1] << 8));
    if (v != 2000 + i) { CHECK(v == 2000 + i); break; }
  }
}

static void test_24_bit_device() {
  Layout l;
  l.subslot = 3; l.bits = 24; l.max_packet = 288;
  FakeAudioHost host(l);
  Rig rig(host);
  UsbAudioDevice* d = rig.dev();
  if (!d) { CHECK(false); return; }
  const Bytes pcm = ramp(960);
  play_all(*d, host, 1, pcm);
  for (const Bytes& p : host.endpoints[1].sent) CHECK(p.size() == 288);
  const Bytes got = payload(host.endpoints[1], 6);
  CHECK(got.size() >= 960 * 6);
  // each sample: a zero low byte and the 16 bits above it
  for (int i = 0; i < 960 && got.size() >= 960 * 6; ++i) {
    const int16_t l16 = static_cast<int16_t>(1 + i), r16 = static_cast<int16_t>(-(1 + i));
    const uint8_t expect[6] = {0, static_cast<uint8_t>(l16), static_cast<uint8_t>(l16 >> 8), 0, static_cast<uint8_t>(r16), static_cast<uint8_t>(r16 >> 8)};
    if (memcmp(&got[6 * i], expect, 6) != 0) { CHECK(false); break; }
  }
}

static void test_uac2_device() {
  Layout l;
  l.version = 2;
  l.interval = 4;       // 1 ms
  FakeAudioHost host(l);
  Rig rig(host);
  UsbAudioDevice* d = rig.dev();
  CHECK(d != nullptr);
  if (!d) return;
  CHECK(host.endpoints[1].interval_us == 1000);
  // the clock was asked what it can do, set to 48 kHz and read back
  CHECK(host.count(0xA1, 0x02, 0x0100, 0x1000) == 1);
  const FakeAudioHost::Control* rate = host.find(0x21, 0x01, 0x0100, 0x1000);
  CHECK(rate && rate->data == Bytes({0x80, 0xBB, 0x00, 0x00}));
  CHECK(host.count(0xA1, 0x01, 0x0100, 0x1000) == 1);
  CHECK(host.count(0x01, 11, 1, 1) == 1);
  CHECK(host.find(0x22, 0x01, 0x0100, 1) == nullptr);   // (UAC1 only)
  // the volume range comes by RANGE, the volume is set, not the unit of a UAC1 device
  CHECK(host.count(0xA1, 0x02, 0x0200, 0x0200) == 1);
  play_all(*d, host, 1, ramp(480));
  for (const Bytes& p : host.endpoints[1].sent) CHECK(p.size() == 192);
  const Bytes got = payload(host.endpoints[1]);
  const Bytes pcm = ramp(480);
  CHECK(got.size() >= pcm.size() && memcmp(got.data(), pcm.data(), pcm.size()) == 0);
}

static void test_uac2_high_rate_interval() {
  Layout l;
  l.version = 2;
  l.interval = 1;       // 125 us: 6 frames, 24 bytes
  l.max_packet = 24;
  FakeAudioHost host(l);
  Rig rig(host);
  UsbAudioDevice* d = rig.dev();
  if (!d) { CHECK(false); return; }
  CHECK(host.endpoints[1].interval_us == 125);
  const Bytes pcm = ramp(1200);
  play_all(*d, host, 1, pcm);
  for (const Bytes& p : host.endpoints[1].sent) CHECK(p.size() == 24);
  const Bytes got = payload(host.endpoints[1]);
  CHECK(got.size() >= pcm.size() && memcmp(got.data(), pcm.data(), pcm.size()) == 0);
  CHECK(host.endpoints[1].pending <= 192);
}

static void test_wrong_rate_rejected() {
  {
    Layout l;
    l.rate = 44100;
    FakeAudioHost host(l);
    Rig rig(host);
    CHECK(rig.devices.count() == 0);
    CHECK(host.started.empty() && host.set_configurations == 0);
    CHECK(rig.alloc.live == 0);
  }
  {
    Layout l;
    l.version = 2;
    l.interval = 4;
    FakeAudioHost host(l);
    host.clock_rate = 44100;                 // the clock can only do 44.1 kHz
    Rig rig(host);
    CHECK(rig.devices.count() == 0);
    CHECK(host.count(0x01, 11, 1, 1) == 0);  // never switched on
    CHECK(rig.alloc.live == 0);
  }
  {
    Layout l;                                // a host without isochronous transfers (UHCI, OHCI, EHCI here) leaves the device alone
    FakeAudioHost host(l);
    host.isochronous = false;
    Rig rig(host);
    CHECK(rig.devices.count() == 0 && host.set_configurations == 0);
  }
  {
    Layout l;                                // endpoint refused by the controller
    FakeAudioHost host(l);
    host.refuse_iso = true;
    Rig rig(host);
    CHECK(rig.devices.count() == 0 && rig.alloc.live == 0);
  }
  {
    Layout l;                                // a packet that is too small for 48 kHz
    l.max_packet = 100;
    FakeAudioHost host(l);
    Rig rig(host);
    CHECK(rig.devices.count() == 0);
    CHECK(host.count(0x01, 11, 1, 1) == 0);
  }
}

static void test_volume_mute() {
  Layout l;
  FakeAudioHost host(l);
  Rig rig(host);
  UsbAudioDevice* d = rig.dev();
  if (!d) { CHECK(false); return; }
  host.controls.clear();
  d->set_volume(50);
  const FakeAudioHost::Control* v = host.find(0x21, 0x01, 0x0200, 0x0200);
  CHECK(v && v->data == Bytes({0x00, 0xE2}));   // -7680 = 0xE200
  d->set_volume(100);
  v = nullptr;
  for (const auto& c : host.controls) if (c.value == 0x0200 && c.type == 0x21) v = &c;
  CHECK(v && v->data == Bytes({0x00, 0x00}));
  d->set_volume(0);
  for (const auto& c : host.controls) if (c.value == 0x0200 && c.type == 0x21) v = &c;
  CHECK(v && v->data == Bytes({0x00, 0xB0}));   // the bottom of the range, -80 dB = -20480 = 0xB000
  d->set_volume(80);
  host.controls.clear();
  d->set_muted(true);
  const FakeAudioHost::Control* m = host.find(0x21, 0x01, 0x0100, 0x0200);
  CHECK(m && m->data == Bytes({1}));
  host.controls.clear();
  d->set_muted(true);
  CHECK(host.controls.empty());   // nothing changed, nothing sent
  d->set_muted(false);
  m = host.find(0x21, 0x01, 0x0100, 0x0200);
  CHECK(m && m->data == Bytes({0}));
}

static void test_software_gain() {
  Layout l;
  l.feature_unit = false;
  FakeAudioHost host(l);
  Rig rig(host);
  UsbAudioDevice* d = rig.dev();
  if (!d) { CHECK(false); return; }
  d->set_volume(50);
  Bytes pcm;
  for (int i = 0; i < 480; ++i) { pcm.push_back(0xE8); pcm.push_back(0x03); pcm.push_back(0x18); pcm.push_back(0xFC); }   // 1000, -1000
  play_all(*d, host, 1, pcm);
  const Bytes got = payload(host.endpoints[1]);
  CHECK(got.size() >= 4 && static_cast<int16_t>(got[0] | (got[1] << 8)) == 500 && static_cast<int16_t>(got[2] | (got[3] << 8)) == -500);
  d->set_muted(true);
  host.endpoints[1].sent.clear();
  play_all(*d, host, 1, pcm);
  const Bytes quiet = payload(host.endpoints[1]);
  CHECK(quiet.empty());   // muted: everything is silence
  CHECK(host.count(0x21, 0x01, 0x0200, 0x0200) == 0);   // there was no feature unit to ask
}

static void test_volume_without_range() {
  Layout l;
  FakeAudioHost host(l);
  host.vol_min = 0; host.vol_max = 0;   // a unit that claims volume but gives no range: scaled in software instead
  Rig rig(host);
  UsbAudioDevice* d = rig.dev();
  if (!d) { CHECK(false); return; }
  d->set_volume(50);
  Bytes pcm;
  for (int i = 0; i < 480; ++i) { pcm.push_back(0xE8); pcm.push_back(0x03); pcm.push_back(0xE8); pcm.push_back(0x03); }
  play_all(*d, host, 1, pcm);
  const Bytes got = payload(host.endpoints[1]);
  CHECK(got.size() >= 4 && static_cast<int16_t>(got[0] | (got[1] << 8)) == 500);
}

static void test_feedback() {
  Layout l;
  l.sync_attr = 0x05;     // asynchronous
  l.feedback = true;
  l.max_packet = 200;
  FakeAudioHost host(l);
  Rig rig(host);
  UsbAudioDevice* d = rig.dev();
  if (!d) { CHECK(false); return; }
  CHECK(host.endpoints[0x82].running);   // the feedback endpoint was started
  // The device says it needs 48.048 frames per millisecond (10.14 fixed point, 3 bytes).
  const uint32_t value = static_cast<uint32_t>(48.048 * 16384);
  FakeAudioHost::Endpoint& fb = host.endpoints[0x82];
  fb.incoming.push_back(Bytes({static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value >> 16)}));
  const Bytes pcm = ramp(48048);
  play_all(*d, host, 1, pcm);
  uint64_t frames = 0;
  uint32_t big = 0, small = 0;
  const FakeAudioHost::Endpoint& e = host.endpoints[1];
  for (size_t i = 0; i < e.sent.size(); ++i) {
    const uint32_t n = static_cast<uint32_t>(e.sent[i].size() / 4);
    CHECK(n == 48 || n == 49 || (i < 8 && n == 48));
    big += n == 49;
    small += n == 48;
  }
  for (const Bytes& p : e.sent) frames += p.size() / 4;
  // roughly one packet in 21 carries the extra frame, from the moment the value arrived
  CHECK(big >= 40 && big <= 60);
  CHECK(frames > 0 && small > big);
  // an unbelievable value is ignored
  fb.incoming.push_back(Bytes({0, 0, 0x40}));   // 256 frames/ms
  const size_t before = e.sent.size();
  play_all(*d, host, 1, ramp(480));
  for (size_t i = before; i < e.sent.size(); ++i) CHECK(e.sent[i].size() / 4 <= 49);
}

static void test_headset_capture() {
  Layout l;
  l.input = true;
  l.in_channels = 1;
  l.in_max_packet = 96;
  FakeAudioHost host(l);
  Rig rig(host);
  UsbAudioDevice* d = rig.dev();
  CHECK(d != nullptr);
  if (!d) return;
  CHECK(d->has_output() && d->has_input());
  IAudioCapture* cap = d->capture();
  CHECK(cap != nullptr);
  if (!cap) return;
  CHECK(host.endpoints[0x81].running && host.count(0x01, 11, 1, 2) == 1);
  FakeAudioHost::Endpoint& in = host.endpoints[0x81];
  Bytes first;
  for (int i = 0; i < 48; ++i) { const int16_t v = static_cast<int16_t>(100 + i); first.push_back(static_cast<uint8_t>(v)); first.push_back(static_cast<uint8_t>(v >> 8)); }
  in.incoming.push_back(first);
  in.incoming.push_back(Bytes());      // a millisecond with nothing: recorded as silence
  in.incoming.push_back(first);
  uint8_t buffer[2048];
  const uint32_t n = cap->read(ByteView(buffer, sizeof buffer));
  CHECK(n == 3 * 48 * 4);
  for (int i = 0; i < 48 && n == 576; ++i) {
    const int16_t left = static_cast<int16_t>(buffer[4 * i] | (buffer[4 * i + 1] << 8)), right = static_cast<int16_t>(buffer[4 * i + 2] | (buffer[4 * i + 3] << 8));
    if (left != 100 + i || right != 100 + i) { CHECK(false); break; }   // mono: both channels
  }
  bool silent = true;
  for (int i = 48 * 4; i < 96 * 4 && n == 576; ++i) silent = silent && buffer[i] == 0;
  CHECK(silent);
  CHECK(cap->available_bytes() == 0);
  // software gain on the way out (no feature unit on the microphone)
  in.incoming.push_back(first);
  cap->set_gain(50);
  const uint32_t m = cap->read(ByteView(buffer, sizeof buffer));
  CHECK(m == 48 * 4 && static_cast<int16_t>(buffer[0] | (buffer[1] << 8)) == 50);
  cap->set_muted(true);
  in.incoming.push_back(first);
  const uint32_t k = cap->read(ByteView(buffer, sizeof buffer));
  CHECK(k == 48 * 4 && buffer[0] == 0 && buffer[1] == 0);
}

static void test_shutdown() {
  Layout l;
  l.input = true;
  FakeAudioHost host(l);
  {
    Rig rig(host);
    CHECK(rig.dev() != nullptr);
    CHECK(rig.alloc.live > 0);
    rig.dev()->shutdown();
    CHECK(host.count(0x01, 11, 0, 1) == 1 && host.count(0x01, 11, 0, 2) == 1);   // back to alternate 0
    CHECK(!host.endpoints[1].running && !host.endpoints[0x81].running);
    CHECK(rig.dev()->write(ConstByteView(ramp(4).data(), 16)) == 0);
    CHECK(rig.dev()->capture() == nullptr);
    rig.dev()->shutdown();   // twice is harmless
    CHECK(host.count(0x01, 11, 0, 1) == 1);
    const long before = rig.alloc.live;
    rig.devices.clear();
    CHECK(rig.alloc.live <= before);
  }
}

static void test_leaks() {
  Layout l;
  l.input = true;
  FakeAudioHost host(l);
  TestAllocator alloc;
  {
    FakeUsb usb;
    usb.hosts.push_back(&host);
    List<UniquePointer<UsbAudioDevice>> devices(alloc);
    UsbAudioDevice::discover(usb, alloc, devices);
    CHECK(devices.count() == 1);
    for (uint64_t i = devices.count(); i > 0; --i) devices[i - 1]->shutdown();
    devices.clear();
  }
  CHECK(alloc.live == 0);
}

static void test_system_audio_devices() {
  FakePci pci;
  FakeClock clock;
  {
    TestAllocator alloc;
    SystemAudioDevices sys(pci, clock, alloc, nullptr, nullptr);
    sys.initialize();
    CHECK(sys.count() == 1 && !sys.is_hardware(0));   // only the software fallback
    sys.shutdown();
  }
  {
    Layout l;
    FakeAudioHost host(l);
    FakeUsb usb;
    usb.hosts.push_back(&host);
    TestAllocator alloc;
    {
      SystemAudioDevices sys(pci, clock, alloc, nullptr, &usb);
      sys.initialize();
      CHECK(sys.count() == 1 && sys.is_hardware(0));
      CHECK(sys.at(0) && strncmp(sys.at(0)->name(), "USB audio", 9) == 0);
      sys.shutdown();
    }
    CHECK(alloc.live == 0);
  }
  {
    // A microphone alone: the software output comes first, so the first device is still something to play to.
    Layout l;
    l.output = false;
    l.input = true;
    FakeAudioHost host(l);
    FakeUsb usb;
    usb.hosts.push_back(&host);
    TestAllocator alloc;
    {
      SystemAudioDevices sys(pci, clock, alloc, nullptr, &usb);
      sys.initialize();
      CHECK(sys.count() == 2);
      CHECK(!sys.is_hardware(0) && sys.is_hardware(1) && !sys.is_hardware(2));
      CHECK(sys.at(1) && sys.at(1)->capture() != nullptr);
      sys.shutdown();
    }
    CHECK(alloc.live == 0);
  }
  {
    // No isochronous support: the device is left alone and the fallback plays.
    Layout l;
    FakeAudioHost host(l);
    host.isochronous = false;
    FakeUsb usb;
    usb.hosts.push_back(&host);
    TestAllocator alloc;
    SystemAudioDevices sys(pci, clock, alloc, nullptr, &usb);
    sys.initialize();
    CHECK(sys.count() == 1 && !sys.is_hardware(0));
    sys.shutdown();
  }
}

int main() {
  test_descriptors_uac1();
  test_descriptors_uac2();
  test_descriptor_fuzz();
  test_uac1_playback();
  test_ring_full_and_order();
  test_mono_device();
  test_24_bit_device();
  test_uac2_device();
  test_uac2_high_rate_interval();
  test_wrong_rate_rejected();
  test_volume_mute();
  test_software_gain();
  test_volume_without_range();
  test_feedback();
  test_headset_capture();
  test_shutdown();
  test_leaks();
  test_system_audio_devices();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("UsbAudioTest: all passed\n");
  return 0;
}
