// Host-side tests for USB video class (webcam) support: the descriptor reader (also on truncated and corrupted descriptors), the standard
// JPEG tables, and UvcCamera against a fake host controller with a fake camera behind it: mode lists, probe/commit and the alternate setting
// chosen, picture assembly from isochronous payloads (frame ID, end of frame, errors, lost packets), Motion JPEG and uncompressed pictures
// read back, picture controls, shutdown, and SystemCameras.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "FakeUsbCamera.hpp"

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct Rig {
  TestAllocator alloc;
  List<UniquePointer<UvcCamera>> cameras{alloc};
  FakeUsb usb;
  explicit Rig(FakeCameraHost& host) { usb.hosts.push_back(&host); UvcCamera::discover(usb, alloc, cameras); }
  UvcCamera* cam() { return cameras.count() ? cameras[0].get() : nullptr; }
};

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
