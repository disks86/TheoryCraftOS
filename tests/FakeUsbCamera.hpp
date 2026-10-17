// Shared by the USB video tests: a counting allocator, UVC configuration descriptors and a fake host controller with a camera behind it that
// records the control requests and hands out the isochronous packets the test feeds it.
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <vector>
#include "Hardware/Camera/SystemCameras.hpp"
#include "Hardware/Camera/Usb/UvcCamera.hpp"
#include "Hardware/Camera/Usb/UvcDescriptors.hpp"
#include "Hardware/Camera/Usb/UvcMjpeg.hpp"
#include "Hardware/VideoCodec/Software/Jpeg.hpp"

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

typedef std::vector<uint8_t> Bytes;
static void put(Bytes& v, std::initializer_list<uint8_t> b) { v.insert(v.end(), b.begin(), b.end()); }
static void put16(Bytes& v, uint32_t x) { v.push_back(static_cast<uint8_t>(x)); v.push_back(static_cast<uint8_t>(x >> 8)); }
static void put32(Bytes& v, uint32_t x) { put16(v, x); put16(v, x >> 16); }

// ------------------------------------------------------------------------------------------------ configuration builder

struct FrameSpec {
  uint16_t width, height;
  std::vector<uint32_t> intervals;      // 100 ns; empty with min/max/step: continuous
  uint32_t min = 0, max = 0, step = 0;
};
struct FormatSpec {
  const char* fourcc;                   // nullptr: Motion JPEG
  std::vector<FrameSpec> frames;
};
struct Layout {
  uint16_t uvc = 0x0110;
  std::vector<FormatSpec> formats;
  std::vector<uint16_t> alternates = {128, 512, 1024};   // wMaxPacketSize of alternate settings 1, 2, ...
  uint8_t interval = 1;
  bool processing_unit = true;
  bool bulk = false;                    // a bulk endpoint on alternate 0 (and no isochronous ones)
  bool high_bandwidth_last = false;     // the last alternate setting asks for 3 transactions per interval
  Layout() {
    formats.push_back({nullptr, {{640, 480, {333333, 666666}}, {1280, 720, {333333}}}});
    formats.push_back({"YUY2", {{16, 8, {333333}}, {640, 480, {666666}}}});
  }
};

static Bytes build_config(const Layout& l) {
  Bytes c;
  put(c, {9, 2, 0, 0, 2, 1, 0, 0x80, 50});
  // video control interface
  put(c, {9, 4, 0, 0, 0, 0x0E, 1, 0, 0});
  put(c, {13, 0x24, 1, static_cast<uint8_t>(l.uvc), static_cast<uint8_t>(l.uvc >> 8), 0, 0, 0, 0, 0, 0, 1, 1});
  put(c, {18, 0x24, 2, 1, 0x01, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0});          // camera terminal
  if (l.processing_unit) put(c, {12, 0x24, 5, 3, 1, 0, 0, 2, 0x1B, 0x00, 0, 0});    // brightness, contrast, saturation, sharpness
  put(c, {9, 0x24, 3, 4, 1, 1, 0, 3, 0});
  // streaming interface, alternate 0
  put(c, {9, 4, 1, 0, static_cast<uint8_t>(l.bulk ? 1 : 0), 0x0E, 2, 0, 0});
  put(c, {static_cast<uint8_t>(13 + l.formats.size()), 0x24, 1, static_cast<uint8_t>(l.formats.size()), 0, 0, 0x81, 0, 4, 0, 0, 0, 1});
  for (size_t i = 0; i < l.formats.size(); ++i) c.push_back(0);
  uint8_t index = 1;
  for (const FormatSpec& f : l.formats) {
    const bool jpeg = f.fourcc == nullptr;
    if (jpeg) put(c, {11, 0x24, 6, index, static_cast<uint8_t>(f.frames.size()), 1, 1, 0, 0, 0, 0});
    else {
      put(c, {27, 0x24, 4, index, static_cast<uint8_t>(f.frames.size())});
      for (int i = 0; i < 4; ++i) c.push_back(static_cast<uint8_t>(f.fourcc[i]));
      put(c, {0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71});
      put(c, {16, 1, 0, 0, 0, 0});
    }
    uint8_t frame_index = 1;
    for (const FrameSpec& fr : f.frames) {
      const size_t n = fr.intervals.size();
      const size_t length = 26 + (n ? 4 * n : 12);
      put(c, {static_cast<uint8_t>(length), 0x24, static_cast<uint8_t>(jpeg ? 7 : 5), frame_index++, 0});
      put16(c, fr.width); put16(c, fr.height);
      put32(c, 1000000); put32(c, 50000000);
      put32(c, jpeg ? fr.width * fr.height : fr.width * fr.height * 2);
      put32(c, n ? fr.intervals[0] : fr.min);
      c.push_back(static_cast<uint8_t>(n));
      if (n) for (uint32_t v : fr.intervals) put32(c, v);
      else { put32(c, fr.min); put32(c, fr.max); put32(c, fr.step); }
    }
    ++index;
  }
  if (!l.bulk) {
    if (l.formats.size() > 0) {
      for (size_t a = 0; a < l.alternates.size(); ++a) {
        put(c, {9, 4, 1, static_cast<uint8_t>(a + 1), 1, 0x0E, 2, 0, 0});
        uint16_t mp = l.alternates[a];
        if (l.high_bandwidth_last && a + 1 == l.alternates.size()) mp |= 2 << 11;
        put(c, {7, 5, 0x81, 0x05, static_cast<uint8_t>(mp), static_cast<uint8_t>(mp >> 8), l.interval});
      }
    }
  } else {
    put(c, {7, 5, 0x81, 0x02, 0x00, 0x02, 0});
  }
  c[2] = static_cast<uint8_t>(c.size());
  c[3] = static_cast<uint8_t>(c.size() >> 8);
  return c;
}

// ------------------------------------------------------------------------------------------------ the fake host

struct FakeCameraHost final : IUsbHostController {
  struct Control { uint8_t type, request; uint16_t value, index; Bytes data; };
  struct Endpoint {
    uint32_t interval_us = 0, max_packet = 0;
    std::vector<Bytes> incoming;
    size_t taken = 0;
    bool running = false;
  };
  Bytes config;
  bool isochronous = true;
  bool refuse_iso = false;
  std::vector<Control> controls;
  std::map<uint8_t, Endpoint> endpoints;
  std::vector<uint8_t> started, stopped;
  Bytes probe = Bytes(48, 0);
  uint32_t payload = 400, max_frame = 0;
  std::map<uint32_t, uint32_t> payload_for;   // (format << 8 | frame) -> payload
  bool change_frame = false;                  // the camera answers the probe with another frame index
  int16_t value_min[16] = {}, value_max[16] = {}, value_cur[16] = {};

  explicit FakeCameraHost(const Layout& l) : config(build_config(l)) {
    value_min[2] = -64; value_max[2] = 64; value_cur[2] = 0;           // brightness (signed)
    value_min[3] = 0; value_max[3] = 200; value_cur[3] = 100;          // contrast
    value_min[7] = 0; value_max[7] = 100; value_cur[7] = 50;           // saturation
    value_min[8] = 0; value_max[8] = 6; value_cur[8] = 3;              // sharpness
  }

  const char* name() const override { return "fake host"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  UsbControllerKind kind() const override { return UsbControllerKind::Xhci; }
  uint32_t port_count() const override { return 1; }
  bool port_info(uint32_t, UsbPortInfo* out) const override { out->connected = true; out->speed = UsbSpeed::High; return true; }
  bool device_descriptor(uint32_t, UsbDeviceDescriptor* out) const override {
    memset(out, 0, sizeof *out);
    out->length = 18; out->descriptor_type = 1; out->vendor_id = 0x046d; out->product_id = 0x0825; out->configuration_count = 1;
    return true;
  }
  bool configuration_descriptor(uint32_t, ByteView buffer, uint16_t* length) const override {
    const size_t n = config.size() < buffer.size() ? config.size() : buffer.size();
    memcpy(buffer.data(), config.data(), n);
    *length = static_cast<uint16_t>(config.size());
    return true;
  }
  bool set_configuration(uint32_t, uint8_t) override { return true; }
  bool control_out(uint32_t, uint8_t type, uint8_t request, uint16_t value, uint16_t index) override {
    controls.push_back({type, request, value, index, {}});
    return true;
  }
  bool control_out_data(uint32_t, uint8_t type, uint8_t request, uint16_t value, uint16_t index, ConstByteView data) override {
    controls.push_back({type, request, value, index, Bytes(data.data(), data.data() + data.size())});
    if (type == 0x21 && request == 0x01 && index == 1 && (value >> 8) == 1) {   // SET_CUR probe
      probe.assign(48, 0);
      memcpy(probe.data(), data.data(), data.size() < 48 ? data.size() : 48);
    }
    if (type == 0x21 && request == 0x01 && (index & 0xFF) == 0 && (index >> 8) == 3) {   // processing unit
      const uint8_t selector = value >> 8;
      if (selector < 16 && data.size() >= 2) value_cur[selector] = static_cast<int16_t>(data[0] | (data[1] << 8));
    }
    return true;
  }
  bool control_in(uint32_t, uint8_t type, uint8_t request, uint16_t value, uint16_t index, ByteView buffer) override {
    memset(buffer.data(), 0, buffer.size());
    controls.push_back({type, request, value, index, {}});
    if (type != 0xA1) return false;
    const uint8_t selector = value >> 8;
    if (index == 1 && selector == 1 && request == 0x81) {   // GET_CUR probe
      Bytes answer = probe;
      memcpy(answer.data() + 18, &max_frame, 4);
      uint32_t p = payload;
      auto it = payload_for.find((uint32_t{probe[2]} << 8) | probe[3]);
      if (it != payload_for.end()) p = it->second;
      memcpy(answer.data() + 22, &p, 4);
      if (change_frame) answer[3] = static_cast<uint8_t>(answer[3] + 1);
      memcpy(buffer.data(), answer.data(), buffer.size() < 48 ? buffer.size() : 48);
      return true;
    }
    if ((index >> 8) == 3 && (index & 0xFF) == 0 && selector < 16 && buffer.size() >= 2) {
      const int16_t* table = request == 0x82 ? value_min : request == 0x83 ? value_max : request == 0x81 ? value_cur : nullptr;
      if (!table) return false;
      memcpy(buffer.data(), &table[selector], 2);
      return true;
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
    e.interval_us = 125u << (interval - 1);
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
  bool iso_receive(uint32_t, uint8_t address, ByteView out, uint16_t* length) override {
    Endpoint& e = endpoints[address];
    if (e.taken >= e.incoming.size()) return false;
    const Bytes& p = e.incoming[e.taken++];
    const size_t n = p.size() < out.size() ? p.size() : out.size();
    if (n) memcpy(out.data(), p.data(), n);
    *length = static_cast<uint16_t>(n);
    return true;
  }

  const Control* find(uint8_t type, uint8_t request, uint16_t value, uint16_t index) const {
    for (const Control& c : controls)
      if (c.type == type && c.request == request && c.value == value && c.index == index) return &c;
    return nullptr;
  }
  size_t count(uint8_t type, uint8_t request, uint16_t value, uint16_t index) const {
    size_t n = 0;
    for (const Control& c : controls) n += c.type == type && c.request == request && c.value == value && c.index == index;
    return n;
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

static int find_mode(ICamera& c, uint32_t w, uint32_t h, uint32_t fps, VideoCodec codec) {
  for (uint32_t i = 0; i < c.mode_count(); ++i) {
    CameraMode m;
    c.mode(i, &m);
    if (m.width == w && m.height == h && m.fps == fps && m.codec == codec) return static_cast<int>(i);
  }
  return -1;
}

// A picture cut into payloads of at most `chunk` bytes, each with the two byte header.
static std::vector<Bytes> packetize(const Bytes& frame, uint8_t fid, size_t chunk, bool eof = true, uint8_t extra = 0) {
  std::vector<Bytes> out;
  size_t at = 0;
  do {
    const size_t n = frame.size() - at < chunk ? frame.size() - at : chunk;
    const bool last = at + n == frame.size();
    Bytes p = {2, static_cast<uint8_t>(fid | (last && eof ? 2 : 0) | extra)};
    p.insert(p.end(), frame.begin() + at, frame.begin() + at + n);
    out.push_back(p);
    at += n;
  } while (at < frame.size());
  return out;
}

static void feed(FakeCameraHost& host, uint8_t endpoint, const std::vector<Bytes>& packets) {
  FakeCameraHost::Endpoint& e = host.endpoints[endpoint];
  e.incoming.insert(e.incoming.end(), packets.begin(), packets.end());
}

