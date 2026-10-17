// Host-side tests for USB video class (webcam) support: the descriptor reader (also on truncated and corrupted descriptors), the standard
// JPEG tables, and UvcCamera against a fake host controller with a fake camera behind it: mode lists, probe/commit and the alternate setting
// chosen, picture assembly from isochronous payloads (frame ID, end of frame, errors, lost packets), Motion JPEG and uncompressed pictures
// read back, picture controls, shutdown, and SystemCameras.
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
  std::vector<FakeCameraHost*> hosts;
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

struct Rig {
  TestAllocator alloc;
  List<UniquePointer<UvcCamera>> cameras{alloc};
  FakeUsb usb;
  explicit Rig(FakeCameraHost& host) { usb.hosts.push_back(&host); UvcCamera::discover(usb, alloc, cameras); }
  UvcCamera* cam() { return cameras.count() ? cameras[0].get() : nullptr; }
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

// ------------------------------------------------------------------------------------------------ pictures

// A grey 16x16 Motion JPEG with no Huffman tables: the standard ones are meant. One component, four blocks, each a DC difference of 0 and an end of block.
static Bytes grey_jpeg_without_tables() {
  Bytes j;
  put(j, {0xFF, 0xD8});
  put(j, {0xFF, 0xDB, 0x00, 0x43, 0x00});
  for (int i = 0; i < 64; ++i) j.push_back(1);
  put(j, {0xFF, 0xC0, 0x00, 0x0B, 0x08, 0x00, 0x10, 0x00, 0x10, 0x01, 0x01, 0x11, 0x00});
  put(j, {0xFF, 0xDA, 0x00, 0x08, 0x01, 0x01, 0x00, 0x00, 0x3F, 0x00});
  put(j, {0x28, 0xA2, 0x8A});
  put(j, {0xFF, 0xD9});
  return j;
}

static bool grey_close(uint32_t pixel, int expect, int tolerance) {
  for (int s = 0; s <= 16; s += 8) {
    const int v = (pixel >> s) & 0xFF;
    if (v < expect - tolerance || v > expect + tolerance) return false;
  }
  return true;
}

// ------------------------------------------------------------------------------------------------ tests

static void test_descriptors() {
  Layout l;
  const Bytes c = build_config(l);
  UvcDevice m;
  CHECK(parse_uvc_configuration(ConstByteView(c.data(), c.size()), &m));
  CHECK(m.uvc_version == 0x0110 && uvc_probe_length(m.uvc_version) == 34 && uvc_probe_length(0x0100) == 26 && uvc_probe_length(0x0150) == 48);
  CHECK(m.have_control && m.control_interface == 0 && m.camera_terminal);
  CHECK(m.processing_unit == 3 && m.processing_controls == 0x1B);
  CHECK(m.stream.interface == 1 && m.stream.format_count == 2 && m.stream.alternate_count == 3);
  const UvcFormat& jpeg = m.stream.formats[0];
  CHECK(jpeg.kind == UvcFormatKind::Mjpeg && jpeg.index == 1 && jpeg.frame_count == 2);
  CHECK(jpeg.frames[0].width == 640 && jpeg.frames[0].height == 480 && jpeg.frames[0].interval_count == 2 && jpeg.frames[0].intervals[1] == 666666);
  const UvcFormat& yuy2 = m.stream.formats[1];
  CHECK(yuy2.kind == UvcFormatKind::Yuy2 && yuy2.index == 2 && yuy2.frame_count == 2 && yuy2.frames[0].width == 16);
  CHECK(m.stream.alternates[0].alternate == 1 && m.stream.alternates[0].endpoint == 0x81 && m.stream.alternates[0].max_packet == 128);
  CHECK(m.stream.alternates[2].alternate == 3 && m.stream.alternates[2].max_packet == 1024);
  CHECK(!m.stream.bulk && !m.stream.high_bandwidth);

  Layout other;                      // no processing unit, other formats, continuous rates, bulk
  other.processing_unit = false;
  other.formats.clear();
  FrameSpec continuous{320, 240, {}, 333333, 2000000, 100000};
  other.formats.push_back({"NV12", {continuous}});
  other.formats.push_back({"UYVY", {{16, 8, {400000}}}});
  other.formats.push_back({"I420", {{16, 8, {400000}}}});
  other.formats.push_back({"XXXX", {{16, 8, {400000}}}});   // unknown: dropped, and its frames with it
  const Bytes c2 = build_config(other);
  CHECK(parse_uvc_configuration(ConstByteView(c2.data(), c2.size()), &m));
  CHECK(m.processing_unit == 0 && m.stream.format_count == 3);
  CHECK(m.stream.formats[0].kind == UvcFormatKind::Nv12 && m.stream.formats[0].frames[0].interval_count == 0 && m.stream.formats[0].frames[0].interval_max == 2000000);
  CHECK(m.stream.formats[1].kind == UvcFormatKind::Uyvy && m.stream.formats[2].kind == UvcFormatKind::I420);

  Layout bulk;
  bulk.bulk = true;
  const Bytes c3 = build_config(bulk);
  CHECK(!parse_uvc_configuration(ConstByteView(c3.data(), c3.size()), &m) || (m.stream.bulk && m.stream.alternate_count == 0));
  Layout high;
  high.high_bandwidth_last = true;
  const Bytes c4 = build_config(high);
  CHECK(parse_uvc_configuration(ConstByteView(c4.data(), c4.size()), &m) && m.stream.high_bandwidth && m.stream.alternate_count == 2);

  Bytes empty;
  CHECK(!parse_uvc_configuration(ConstByteView(empty.data(), 0), &m));
}

static void test_descriptor_fuzz() {
  uint32_t seed = 7;
  auto rnd = [&] { seed = seed * 1664525 + 1013904223; return seed >> 8; };
  Layout a;
  Layout b;
  b.formats.clear();
  b.formats.push_back({"NV12", {{320, 240, {}, 333333, 2000000, 100000}}});
  b.uvc = 0x0150;
  for (const Layout* l : {&a, &b}) {
    const Bytes base = build_config(*l);
    for (int round = 0; round < 4000; ++round) {
      Bytes c = base;
      switch (round % 3) {
        case 0: c.resize(rnd() % (c.size() + 1)); break;
        case 1: for (int k = 0; k < 1 + static_cast<int>(rnd() % 4); ++k) c[rnd() % c.size()] ^= static_cast<uint8_t>(1u << (rnd() % 8)); break;
        case 2: for (int k = 0; k < 1 + static_cast<int>(rnd() % 3); ++k) c[rnd() % c.size()] = static_cast<uint8_t>(rnd()); break;
      }
      std::vector<uint8_t> exact(c);   // exactly sized so the sanitizer sees any read past the end
      UvcDevice m;
      parse_uvc_configuration(ConstByteView(exact.data(), exact.size()), &m);
      if (round % 40 == 0) {   // and through the whole camera, started with every mode
        FakeCameraHost host(*l);
        host.config = exact;
        Rig rig(host);
        if (rig.cam()) {
          for (uint32_t i = 0; i < rig.cam()->mode_count(); ++i) {
            if (rig.cam()->start_mode(i)) {
              feed(host, 0x81, packetize(Bytes(300, 0x55), 0, 100));
              feed(host, 0x81, packetize(Bytes(300, 0x56), 1, 100));
              rig.cam()->pump();
            }
          }
          rig.cam()->stop();
        }
      }
    }
  }
}

static void test_standard_tables() {
  const ConstByteView t = uvc_standard_huffman_tables();
  CHECK(t.size() == kUvcHuffmanBytes);
  size_t at = 0;
  int tables = 0;
  while (at + 4 < t.size()) {
    CHECK(t[at] == 0xFF && t[at + 1] == 0xC4);
    const size_t length = (t[at + 2] << 8) | t[at + 3];
    const uint8_t id = t[at + 4];
    size_t total = 0;
    double kraft = 0;
    for (int len = 1; len <= 16; ++len) {
      total += t[at + 4 + len];
      for (int k = 0; k < t[at + 4 + len]; ++k) kraft += 1.0 / (1u << len);
    }
    CHECK(length == 2 + 1 + 16 + total);
    CHECK(kraft < 1.0);               // a prefix code with room left (the all-ones code is never used)
    bool seen[256] = {};
    for (size_t i = 0; i < total; ++i) {
      CHECK(!seen[t[at + 21 + i]]);
      seen[t[at + 21 + i]] = true;
    }
    if (id == 0x00 || id == 0x01) CHECK(total == 12);
    else CHECK(total == 162 && (id == 0x10 || id == 0x11));
    at += 2 + length;
    ++tables;
  }
  CHECK(tables == 4 && at == t.size());

  Bytes j = grey_jpeg_without_tables();
  Bytes room = j;
  room.resize(j.size() + kUvcHuffmanBytes);
  const uint32_t n = uvc_complete_jpeg(ByteView(room.data(), room.size()), static_cast<uint32_t>(j.size()));
  CHECK(n == j.size() + kUvcHuffmanBytes);
  CHECK(room[0] == 0xFF && room[1] == 0xD8 && room[2] == 0xFF && room[3] == 0xC4);
  CHECK(memcmp(&room[2 + kUvcHuffmanBytes], &j[2], j.size() - 2) == 0);
  // already complete: left alone
  Bytes again(room.begin(), room.begin() + n);
  again.resize(n + kUvcHuffmanBytes);
  CHECK(uvc_complete_jpeg(ByteView(again.data(), again.size()), n) == n);
  // not a JPEG, or no room, or no scan
  Bytes junk(100, 0x42);
  CHECK(uvc_complete_jpeg(ByteView(junk.data(), junk.size()), 100) == 0);
  CHECK(uvc_complete_jpeg(ByteView(j.data(), j.size()), static_cast<uint32_t>(j.size())) == 0);
  Bytes noscan = {0xFF, 0xD8, 0xFF, 0xDB, 0x00, 0x03, 0x00, 0xFF, 0xD9};
  noscan.resize(noscan.size() + kUvcHuffmanBytes);
  CHECK(uvc_complete_jpeg(ByteView(noscan.data(), noscan.size()), 9) == 0);
}

static void test_modes() {
  Layout l;
  l.formats.push_back({"NV12", {{320, 240, {}, 333333, 2000000, 100000}}});
  FakeCameraHost host(l);
  Rig rig(host);
  UvcCamera* c = rig.cam();
  CHECK(c != nullptr);
  if (!c) return;
  CHECK(strcmp(c->name(), "USB camera 046d:0825") == 0);
  CHECK(find_mode(*c, 640, 480, 30, VideoCodec::Mjpeg) >= 0);
  CHECK(find_mode(*c, 640, 480, 15, VideoCodec::Mjpeg) >= 0);
  CHECK(find_mode(*c, 1280, 720, 30, VideoCodec::Mjpeg) >= 0);
  CHECK(find_mode(*c, 16, 8, 30, VideoCodec::Raw) >= 0);
  CHECK(find_mode(*c, 640, 480, 15, VideoCodec::Raw) >= 0);
  CHECK(find_mode(*c, 320, 240, 30, VideoCodec::Raw) >= 0);   // from the continuous range: 30, 25, 24, 20, 15, 10, 5 within [3.33 ms.. 200 ms]
  CHECK(find_mode(*c, 320, 240, 5, VideoCodec::Raw) >= 0);
  CHECK(find_mode(*c, 320, 240, 60, VideoCodec::Raw) < 0);
  CHECK(c->mode_count() >= 10);
  CameraMode m;
  CHECK(!c->mode(c->mode_count(), &m));
  CHECK(!c->streaming() && c->frame_format().width == 0);
  CHECK(c->has_control(CameraControl::Brightness) && c->has_control(CameraControl::Contrast) && c->has_control(CameraControl::Saturation) && c->has_control(CameraControl::Sharpness));
}

static void test_start_and_probe() {
  Layout l;
  FakeCameraHost host(l);
  host.max_frame = 60000;
  Rig rig(host);
  UvcCamera* c = rig.cam();
  if (!c) { CHECK(false); return; }
  const int mode = find_mode(*c, 640, 480, 15, VideoCodec::Mjpeg);
  CHECK(mode >= 0 && c->start_mode(static_cast<uint32_t>(mode)));
  CHECK(c->streaming() && c->frame_format().width == 640 && c->frame_format().height == 480);
  CHECK(!c->limited_range());
  // probe: set (format 1, frame 1, 66.6666 ms), read back; commit with the answer
  const FakeCameraHost::Control* set = host.find(0x21, 0x01, 0x0100, 1);
  CHECK(set && set->data.size() == 34 && set->data[0] == 1 && set->data[2] == 1 && set->data[3] == 1);
  CHECK(set && memcmp(&set->data[4], "\x2A\x2C\x0A\x00", 4) == 0);   // 666666 = 0x0A2C2A
  const FakeCameraHost::Control* commit = host.find(0x21, 0x01, 0x0200, 1);
  CHECK(commit && commit->data.size() == 34 && commit->data[2] == 1 && commit->data[3] == 1);
  uint32_t committed_payload = 0;
  if (commit) memcpy(&committed_payload, &commit->data[22], 4);
  CHECK(committed_payload == 400);
  // the smallest alternate setting that holds 400 bytes: 512 = alternate 2
  CHECK(host.count(0x01, 11, 2, 1) == 1 && host.count(0x01, 11, 1, 1) == 0 && host.count(0x01, 11, 3, 1) == 0);
  CHECK(host.endpoints[0x81].running && host.endpoints[0x81].max_packet == 512 && host.endpoints[0x81].interval_us == 125);
  // a second start first stops the first
  const int second = find_mode(*c, 1280, 720, 30, VideoCodec::Mjpeg);
  host.payload_for[(1u << 8) | 2] = 100;
  CHECK(c->start_mode(static_cast<uint32_t>(second)));
  CHECK(host.count(0x01, 11, 0, 1) == 1);   // switched off in between
  CHECK(host.count(0x01, 11, 1, 1) == 1);   // and the 128 byte setting for 100 bytes
  CHECK(c->frame_format().width == 1280 && c->frame_format().height == 720);
  c->stop();
  CHECK(!c->streaming() && !host.endpoints[0x81].running && host.count(0x01, 11, 0, 1) == 2);
  c->stop();   // twice is harmless
  CHECK(host.count(0x01, 11, 0, 1) == 2);
}

static void test_start_failures() {
  {   // more payload than any alternate setting carries
    Layout l;
    FakeCameraHost host(l);
    host.payload = 3000;
    Rig rig(host);
    UvcCamera* c = rig.cam();
    if (!c) { CHECK(false); return; }
    CHECK(!c->start_mode(0) && !c->streaming());
    CHECK(host.started.empty());
    CHECK(host.count(0x01, 11, 1, 1) + host.count(0x01, 11, 2, 1) + host.count(0x01, 11, 3, 1) == 0);
    CHECK(rig.alloc.live >= 0);
  }
  {   // no payload size at all
    Layout l;
    FakeCameraHost host(l);
    host.payload = 0;
    Rig rig(host);
    CHECK(rig.cam() && !rig.cam()->start_mode(0));
  }
  {   // the camera answers with another frame
    Layout l;
    FakeCameraHost host(l);
    host.change_frame = true;
    Rig rig(host);
    CHECK(rig.cam() && !rig.cam()->start_mode(0) && host.started.empty());
  }
  {   // the controller refuses the endpoint
    Layout l;
    FakeCameraHost host(l);
    host.refuse_iso = true;
    Rig rig(host);
    CHECK(rig.cam() && !rig.cam()->start_mode(0));
    CHECK(rig.cam() && !rig.cam()->start_mode(1000));
  }
  {
    Layout l;                                  // a controller without isochronous transfers
    FakeCameraHost host(l);
    host.isochronous = false;
    Rig rig(host);
    CHECK(rig.cameras.count() == 0);
  }
  {
    Layout l;                                  // streams over a bulk endpoint: not handled, left alone
    l.bulk = true;
    FakeCameraHost host(l);
    Rig rig(host);
    CHECK(rig.cameras.count() == 0);
  }
  {
    Layout l;                                  // only high-bandwidth alternate settings
    l.alternates = {1024};
    l.high_bandwidth_last = true;
    FakeCameraHost host(l);
    Rig rig(host);
    CHECK(rig.cameras.count() == 0);
  }
}

static void test_start_by_size() {
  Layout l;
  FakeCameraHost host(l);
  host.payload_for[(1u << 8) | 2] = 5000;       // 1280x720 does not fit
  Rig rig(host);
  UvcCamera* c = rig.cam();
  if (!c) { CHECK(false); return; }
  CHECK(c->start(640, 480, 30));
  CameraMode m;
  VideoFormat f = c->frame_format();
  CHECK(f.width == 640 && f.height == 480);
  c->stop();
  CHECK(c->start(800, 600, 30) && c->frame_format().width == 640);          // the largest below
  c->stop();
  CHECK(c->start(1920, 1080, 60) && c->frame_format().width == 640);        // 1280x720 was first choice but would not fit
  c->stop();
  CHECK(c->start(640, 480, 20) && c->frame_format().width == 640);          // nearest rate below 20: 15
  const int active = find_mode(*c, 640, 480, 15, VideoCodec::Mjpeg);
  CHECK(active >= 0 && host.find(0x21, 0x01, 0x0100, 1) != nullptr);
  c->stop();
  CHECK(c->start(16, 8, 30) && c->frame_format().width == 16);              // the small raw mode: the only one that fits
  CHECK(c->limited_range());
  c->stop();
  CHECK(c->start(0, 0, 0) || true);                                         // "anything" does not crash
  c->stop();
  CHECK(!c->mode(999, &m));
}

static void test_mjpeg_frames() {
  Layout l;
  FakeCameraHost host(l);
  host.max_frame = 40000;
  Rig rig(host);
  UvcCamera* c = rig.cam();
  if (!c) { CHECK(false); return; }
  VideoFrameBuffer out(rig.alloc);
  // a frame size of the camera's mode is needed: 640x480 modes; the grey picture is 16x16, so decoding fails -> use a 16x16 mode in a custom layout
  Layout small;
  small.formats.clear();
  small.formats.push_back({nullptr, {{16, 16, {333333}}}});
  FakeCameraHost shost(small);
  Rig srig(shost);
  UvcCamera* s = srig.cam();
  if (!s) { CHECK(false); return; }
  VideoFormat bgra;
  bgra.width = 16; bgra.height = 16;
  VideoFrameBuffer picture(srig.alloc);
  CHECK(picture.allocate(bgra));
  CHECK(s->start_mode(0));
  CHECK(!s->read_frame(picture));              // nothing yet
  const Bytes jpeg = grey_jpeg_without_tables();

  // the first, partial picture is ignored: the stream is joined in the middle of picture 0
  feed(shost, 0x81, packetize(Bytes(50, 0), 0, 20, false));
  feed(shost, 0x81, packetize(jpeg, 1, 30));
  feed(shost, 0x81, {Bytes()});                 // an interval with nothing in it
  s->pump();
  CHECK(s->statistics().frames == 1);
  CHECK(s->read_frame(picture));
  uint32_t* px = reinterpret_cast<uint32_t*>(picture.plane(0).data());
  CHECK(grey_close(px[0], 128, 3) && grey_close(px[16 * 16 - 1], 128, 3) && (px[100] >> 24) == 0xFF);
  CHECK(!s->read_frame(picture));              // already read
  CHECK(s->statistics().frames_read == 1);

  // a second picture while the first one is unread: the first is dropped
  feed(shost, 0x81, packetize(jpeg, 0, 100));
  feed(shost, 0x81, packetize(jpeg, 1, 100));
  s->pump();
  CHECK(s->statistics().frames == 3 && s->statistics().frames_dropped == 1);
  CHECK(s->read_frame(picture) && !s->read_frame(picture));

  // the extra packets after the end of a picture are not part of the next one
  {
    std::vector<Bytes> p = packetize(jpeg, 0, 1000);
    Bytes extra = {2, 0x00, 1, 2, 3};
    p.push_back(extra);
    p.push_back(extra);
    feed(shost, 0x81, p);
    s->pump();
    CHECK(s->statistics().frames == 4 && s->statistics().frames_damaged == 0);
    CHECK(s->read_frame(picture));
  }
  // the camera marks an error
  {
    const uint64_t before = s->statistics().frames_damaged;
    feed(shost, 0x81, packetize(jpeg, 1, 30, true, 0x40));
    s->pump();
    CHECK(s->statistics().frames_damaged == before + 1 && !s->read_frame(picture));
  }
  // a picture that never says it ended is taken when the next one starts (Motion JPEG), and when the data is not a JPEG it is damaged
  {
    const uint64_t frames = s->statistics().frames, damaged = s->statistics().frames_damaged;
    feed(shost, 0x81, packetize(jpeg, 0, 30, false));
    feed(shost, 0x81, packetize(Bytes(40, 0x77), 1, 30, false));
    feed(shost, 0x81, packetize(jpeg, 0, 30, true));
    s->pump();
    CHECK(s->statistics().frames == frames + 2 && s->statistics().frames_damaged == damaged + 1);
  }
  // headers that make no sense
  {
    const uint64_t bad = s->statistics().packets_bad, damaged = s->statistics().frames_damaged;
    std::vector<Bytes> p = packetize(jpeg, 1, 30);
    p[1][0] = 200;                              // header longer than the packet
    feed(shost, 0x81, p);
    feed(shost, 0x81, {Bytes({1})});             // a single byte
    s->pump();
    CHECK(s->statistics().packets_bad == bad + 2 && s->statistics().frames_damaged == damaged + 1);
  }
  // a picture too large for the mode is thrown away, the next one is fine
  {
    const uint64_t damaged = s->statistics().frames_damaged;
    feed(shost, 0x81, packetize(Bytes(200000, 0x11), 0, 1000));
    feed(shost, 0x81, packetize(jpeg, 1, 100));
    s->pump();
    CHECK(s->statistics().frames_damaged == damaged + 1);
    CHECK(s->read_frame(picture));
  }
  // a different size from the buffer is refused and does not use up the picture
  {
    VideoFormat wrong;
    wrong.width = 32; wrong.height = 16;
    VideoFrameBuffer other(srig.alloc);
    CHECK(other.allocate(wrong));
    feed(shost, 0x81, packetize(jpeg, 0, 100));
    feed(shost, 0x81, packetize(jpeg, 1, 100));
    s->pump();
    CHECK(!s->read_frame(other));
    CHECK(s->read_frame(picture));
  }
  // YUV out of a JPEG is full range
  {
    VideoFormat i420;
    i420.width = 16; i420.height = 16; i420.pixel_format = PixelFormat::I420;
    VideoFrameBuffer yuv(srig.alloc);
    CHECK(yuv.allocate(i420));
    feed(shost, 0x81, packetize(jpeg, 0, 100));
    feed(shost, 0x81, packetize(jpeg, 1, 100));
    s->pump();
    CHECK(s->read_frame(yuv) && yuv.plane(0)[0] >= 126 && yuv.plane(0)[0] <= 130 && !s->limited_range());
  }
  s->stop();
  CHECK(!s->read_frame(picture));
  s->shutdown();
  (void)c;
}

static void test_mjpeg_with_tables_roundtrip() {
  // a real encoder's output (tables included) goes through untouched and decodes to about the colour that went in
  TestAllocator alloc;
  VideoFormat f;
  f.width = 32; f.height = 16;
  VideoFrameBuffer src(alloc);
  CHECK(src.allocate(f));
  uint32_t* p = reinterpret_cast<uint32_t*>(src.plane(0).data());
  for (uint32_t i = 0; i < 32 * 16; ++i) p[i] = 0xFF3366CC;
  JpegEncoder encoder(alloc);
  CHECK(encoder.encode(src.frame(), 90));
  Bytes jpeg(encoder.output().data(), encoder.output().data() + encoder.output().size());

  Layout l;
  l.formats.clear();
  l.formats.push_back({nullptr, {{32, 16, {333333}}}});
  FakeCameraHost host(l);
  host.max_frame = 20000;
  Rig rig(host);
  UvcCamera* c = rig.cam();
  if (!c) { CHECK(false); return; }
  VideoFrameBuffer out(rig.alloc);
  CHECK(out.allocate(f));
  CHECK(c->start_mode(0));
  feed(host, 0x81, packetize(Bytes(10, 0), 0, 100));
  feed(host, 0x81, packetize(jpeg, 1, 200));
  feed(host, 0x81, packetize(jpeg, 0, 200));
  c->pump();
  CHECK(c->read_frame(out));
  const uint32_t v = reinterpret_cast<uint32_t*>(out.plane(0).data())[40];
  const int r = (v >> 16) & 0xFF, g = (v >> 8) & 0xFF, b = v & 0xFF;
  CHECK(r >= 0x33 - 8 && r <= 0x33 + 8 && g >= 0x66 - 8 && g <= 0x66 + 8 && b >= 0xCC - 8 && b <= 0xCC + 8);
}

static Bytes yuy2_picture(uint32_t w, uint32_t h, uint8_t y, uint8_t u, uint8_t v, bool uyvy) {
  Bytes b;
  for (uint32_t row = 0; row < h; ++row)
    for (uint32_t x = 0; x < w; x += 2) {
      if (uyvy) put(b, {u, y, v, y});
      else put(b, {y, u, y, v});
    }
  return b;
}

static void test_raw_formats() {
  for (int kind = 0; kind < 4; ++kind) {
    const char* names[] = {"YUY2", "UYVY", "NV12", "I420"};
    Layout l;
    l.formats.clear();
    l.formats.push_back({names[kind], {{16, 8, {333333}}}});
    FakeCameraHost host(l);
    Rig rig(host);
    UvcCamera* c = rig.cam();
    if (!c) { CHECK(false); continue; }
    CHECK(c->start_mode(0) && c->limited_range());
    VideoFormat bgra;
    bgra.width = 16; bgra.height = 8;
    VideoFrameBuffer picture(rig.alloc);
    CHECK(picture.allocate(bgra));
    Bytes frame;
    if (kind < 2) frame = yuy2_picture(16, 8, 180, 90, 200, kind == 1);
    else if (kind == 2) { frame = Bytes(16 * 8, 180); frame.resize(16 * 8 + 16 * 4); for (size_t i = 0; i < 16 * 4; i += 2) { frame[16 * 8 + i] = 90; frame[16 * 8 + i + 1] = 200; } }
    else { frame = Bytes(16 * 8, 180); frame.resize(16 * 8 + 8 * 4 * 2); for (int i = 0; i < 32; ++i) { frame[16 * 8 + i] = 90; frame[16 * 8 + 32 + i] = 200; } }
    feed(host, 0x81, packetize(Bytes(8, 0), 1, 100));
    feed(host, 0x81, packetize(frame, 0, 60));
    feed(host, 0x81, packetize(frame, 1, 60));
    c->pump();
    CHECK(c->statistics().frames == 2);
    CHECK(c->read_frame(picture));
    // BT.601 limited range: Y 180, U 90, V 200 -> R = 1.164*(180-16) + 1.596*(200-128) = 305 (clamped 255), G = 190.9 - 0.392*(90-128) - 0.813*(72) = 147, B = 190.9 + 2.017*(-38) = 114
    const uint32_t px = reinterpret_cast<uint32_t*>(picture.plane(0).data())[3 * 16 + 5];
    const int r = (px >> 16) & 0xFF, g = (px >> 8) & 0xFF, b = px & 0xFF;
    CHECK(r >= 250 && g >= 140 && g <= 155 && b >= 106 && b <= 122);
    // in I420 the same samples come back unchanged
    VideoFormat i420 = bgra;
    i420.pixel_format = PixelFormat::I420;
    VideoFrameBuffer yuv(rig.alloc);
    CHECK(yuv.allocate(i420));
    feed(host, 0x81, packetize(frame, 0, 60));
    feed(host, 0x81, packetize(frame, 1, 60));
    c->pump();
    CHECK(c->read_frame(yuv));
    CHECK(yuv.plane(0)[0] == 180 && yuv.plane(0)[16 * 8 - 1] == 180 && yuv.plane(1)[0] == 90 && yuv.plane(2)[0] == 200 && yuv.plane(1)[8 * 4 - 1] == 90);
    // wrong length: damaged
    const uint64_t damaged = c->statistics().frames_damaged;
    Bytes shortened(frame.begin(), frame.end() - 2);
    feed(host, 0x81, packetize(shortened, 0, 60));
    feed(host, 0x81, packetize(frame, 1, 60));
    c->pump();
    CHECK(c->statistics().frames_damaged == damaged + 1);
  }
}

static void test_yuy2_chroma_average() {
  Layout l;
  l.formats.clear();
  l.formats.push_back({"YUY2", {{4, 2, {333333}}}});
  FakeCameraHost host(l);
  Rig rig(host);
  UvcCamera* c = rig.cam();
  if (!c) { CHECK(false); return; }
  CHECK(c->start_mode(0));
  // two rows with different chroma: 100/150 U, 20/40 V; the 4:2:0 result is the average
  Bytes frame;
  put(frame, {10, 100, 11, 20, 12, 100, 13, 20});
  put(frame, {14, 150, 15, 40, 16, 150, 17, 40});
  VideoFormat i420;
  i420.width = 4; i420.height = 2; i420.pixel_format = PixelFormat::I420;
  VideoFrameBuffer out(rig.alloc);
  CHECK(out.allocate(i420));
  feed(host, 0x81, packetize(frame, 1, 100));
  feed(host, 0x81, packetize(frame, 0, 100));
  feed(host, 0x81, packetize(frame, 1, 100));
  c->pump();
  CHECK(c->read_frame(out));
  CHECK(out.plane(0)[0] == 10 && out.plane(0)[1] == 11 && out.plane(0)[2] == 12 && out.plane(0)[3] == 13 && out.plane(0)[4] == 14 && out.plane(0)[7] == 17);
  CHECK(out.plane(1)[0] == 125 && out.plane(1)[1] == 125 && out.plane(2)[0] == 30 && out.plane(2)[1] == 30);
}

static void test_controls() {
  Layout l;
  FakeCameraHost host(l);
  Rig rig(host);
  UvcCamera* c = rig.cam();
  if (!c) { CHECK(false); return; }
  // brightness: signed -64..64
  CHECK(c->get_control(CameraControl::Brightness) == 50);
  CHECK(c->set_control(CameraControl::Brightness, 100) && host.value_cur[2] == 64);
  CHECK(c->set_control(CameraControl::Brightness, 0) && host.value_cur[2] == -64);
  CHECK(c->set_control(CameraControl::Brightness, 25) && host.value_cur[2] == -32);
  CHECK(c->get_control(CameraControl::Brightness) == 25);
  CHECK(c->set_control(CameraControl::Contrast, 50) && host.value_cur[3] == 100);
  CHECK(c->set_control(CameraControl::Sharpness, 200) && host.value_cur[8] == 6);   // clamped to 100 %
  const FakeCameraHost::Control* set = host.find(0x21, 0x01, 0x0200, 0x0300);
  CHECK(set && set->data.size() == 2);
  Layout none;
  none.processing_unit = false;
  FakeCameraHost host2(none);
  Rig rig2(host2);
  UvcCamera* c2 = rig2.cam();
  if (!c2) { CHECK(false); return; }
  CHECK(!c2->has_control(CameraControl::Brightness) && !c2->set_control(CameraControl::Brightness, 50) && c2->get_control(CameraControl::Brightness) == -1);
}

static void test_shutdown_and_leaks() {
  Layout l;
  FakeCameraHost host(l);
  TestAllocator alloc;
  {
    FakeUsb usb;
    usb.hosts.push_back(&host);
    List<UniquePointer<UvcCamera>> cameras(alloc);
    UvcCamera::discover(usb, alloc, cameras);
    CHECK(cameras.count() == 1);
    CHECK(cameras[0]->start(640, 480, 30));
    feed(host, 0x81, packetize(Bytes(300, 1), 0, 100));
    feed(host, 0x81, packetize(Bytes(300, 1), 1, 100));
    cameras[0]->pump();
    cameras[0]->shutdown();
    CHECK(!host.endpoints[0x81].running && host.count(0x01, 11, 0, 1) == 1);
    cameras[0]->shutdown();
    CHECK(!cameras[0]->start_mode(0));        // not configured any more
    cameras.clear();
  }
  CHECK(alloc.live == 0);
  {   // destroyed while streaming
    TestAllocator alloc2;
    FakeCameraHost host2(l);
    {
      FakeUsb usb;
      usb.hosts.push_back(&host2);
      List<UniquePointer<UvcCamera>> cameras(alloc2);
      UvcCamera::discover(usb, alloc2, cameras);
      CHECK(cameras.count() == 1 && cameras[0]->start_mode(0));
    }
    CHECK(alloc2.live == 0 && !host2.endpoints[0x81].running);
  }
}

static void test_system_cameras() {
  Layout l;
  FakeCameraHost host(l);
  FakeUsb usb;
  usb.hosts.push_back(&host);
  TestAllocator alloc;
  {
    SystemCameras cameras(usb, alloc);
    CHECK(cameras.count() == 0 && cameras.at(0) == nullptr);
    cameras.initialize();
    CHECK(cameras.count() == 1 && cameras.at(0) && cameras.at(0)->mode_count() > 0 && !cameras.at(1));
    CHECK(cameras.at(0)->start(640, 480, 30));
    feed(host, 0x81, packetize(Bytes(300, 7), 0, 100));
    feed(host, 0x81, packetize(Bytes(300, 7), 1, 100));
    cameras.pump();
    CHECK(cameras.at(0)->statistics().frames + cameras.at(0)->statistics().frames_damaged == 1);
    cameras.shutdown();
    CHECK(cameras.count() == 0 && !host.endpoints[0x81].running);
  }
  CHECK(alloc.live == 0);
  Layout none;
  none.alternates.clear();
  FakeCameraHost empty(none);
  FakeUsb usb2;
  usb2.hosts.push_back(&empty);
  SystemCameras nothing(usb2, alloc);
  nothing.initialize();
  CHECK(nothing.count() == 0);
}

int main() {
  test_descriptors();
  test_descriptor_fuzz();
  test_standard_tables();
  test_modes();
  test_start_and_probe();
  test_start_failures();
  test_start_by_size();
  test_mjpeg_frames();
  test_mjpeg_with_tables_roundtrip();
  test_raw_formats();
  test_yuy2_chroma_average();
  test_controls();
  test_shutdown_and_leaks();
  test_system_cameras();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("UvcTest: all passed\n");
  return 0;
}
