// Host-side tests for USB camera hot-plug: SystemCameras::poll() against a fake host controller with four ports into which fake UVC cameras
// are plugged and from which they are pulled - also while streaming. A camera that leaves stops delivering pictures at once, one that
// takes its port never inherits the old camera's stream, and cameras are independent of each other.
#include <memory>
#include <string>
#include <vector>
#include "FakeCameraBay.hpp"

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

// A grey 16x16 Motion JPEG with no Huffman tables (the standard ones are meant): see UvcTest.
static Bytes grey_jpeg() {
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

static Layout small_camera() {
  Layout l;
  l.formats.clear();
  l.formats.push_back({nullptr, {{16, 16, {333333, 666666}}}});
  return l;
}

struct Rig {
  TestAllocator& alloc;
  FakeCameraBay bay;
  FakeUsb usb;
  std::vector<std::unique_ptr<FakeCameraHost>> made;
  SystemCameras cameras;
  explicit Rig(TestAllocator& a) : alloc(a), cameras(usb, a) { usb.hosts.push_back(&bay); }
  FakeCameraHost* make(const Layout& l = small_camera()) {
    made.push_back(std::make_unique<FakeCameraHost>(l));
    made.back()->max_frame = 40000;
    return made.back().get();
  }
};

// One picture on the wire, then the first packet of the next one (so the stream is joined at a boundary).
static void send_frames(FakeCameraBay& bay, uint32_t port, int count) {
  const Bytes jpeg = grey_jpeg();
  uint8_t fid = 0;
  bay.send(port, packetize(Bytes(20, 0), fid ^ 1, 100, false));   // (the tail of a picture the stream joined in the middle of)
  for (int i = 0; i < count; ++i) {
    bay.send(port, packetize(jpeg, fid, 100));
    fid ^= 1;
  }
  bay.send(port, packetize(jpeg, fid, 100));
}

static bool read_grey(ICamera& c, TestAllocator& alloc) {
  VideoFormat f;
  f.width = 16; f.height = 16;
  VideoFrameBuffer out(alloc);
  if (!out.allocate(f)) return false;
  if (!c.read_frame(out)) return false;
  const uint32_t v = reinterpret_cast<uint32_t*>(out.plane(0).data())[40];
  for (int s = 0; s <= 16; s += 8) {
    const int channel = (v >> s) & 0xFF;
    if (channel < 124 || channel > 132) return false;
  }
  return true;
}

static void test_plug_stream_unplug() {
  TestAllocator alloc;
  {
    Rig r(alloc);
    r.cameras.initialize();
    CameraEvent e;
    CHECK(r.cameras.count() == 0 && !r.cameras.poll(&e));

    FakeCameraHost* cam = r.make();
    r.bay.plug(1, cam);
    CHECK(r.cameras.poll(&e) && e.kind == CameraEvent::Kind::Added && e.id == 1 && e.camera && e.camera->present() && !e.was_streaming);
    CHECK(strncmp(e.camera->name(), "USB camera", 10) == 0 && e.camera->mode_count() == 2);
    CHECK(r.cameras.count() == 1 && r.cameras.at(0) == e.camera.get() && r.cameras.id(0) == 1 && !r.cameras.poll(&e));
    SharedPointer<ICamera> held = r.cameras.share(0);

    CHECK(held->start(16, 16, 30) && held->streaming());
    send_frames(r.bay, 1, 3);
    r.cameras.pump();
    CHECK(held->statistics().frames >= 3);
    CHECK(read_grey(*held, alloc));

    // pulled out while streaming
    r.bay.unplug(1);
    CHECK(held->streaming() && !held->present());   // (until the removal is noticed it still thinks so, but nothing arrives)
    r.bay.send(1, packetize(grey_jpeg(), 0, 100));
    CHECK(r.cameras.poll(&e) && e.kind == CameraEvent::Kind::Removed && e.id == 1 && e.was_streaming && e.camera.get() == held.get());
    CHECK(!held->streaming() && !held->present() && r.cameras.count() == 0 && r.cameras.at(0) == nullptr && !r.cameras.share(0));
    CHECK(!held->start(16, 16, 30) && !held->start_mode(0));
    VideoFormat f;
    f.width = 16; f.height = 16;
    VideoFrameBuffer out(alloc);
    CHECK(out.allocate(f) && !held->read_frame(out));
    held->pump();   // harmless
    CHECK(!r.cameras.poll(&e));
    e = CameraEvent();
    held.reset();
    r.cameras.shutdown();
  }
  CHECK(alloc.live == 0);
}

static void test_camera_at_start_up() {
  TestAllocator alloc;
  {
    Rig r(alloc);
    FakeCameraHost* boot = r.make();
    r.bay.plug(2, boot);
    r.cameras.initialize();
    CHECK(r.cameras.count() == 1);
    CameraEvent e;
    CHECK(!r.cameras.poll(&e));   // (it was there at start-up: not new)
    r.bay.unplug(2);
    CHECK(r.cameras.poll(&e) && e.kind == CameraEvent::Kind::Removed && !e.was_streaming && r.cameras.count() == 0);
    e = CameraEvent();
    r.cameras.shutdown();
  }
  CHECK(alloc.live == 0);
}

static void test_replaced_before_noticed() {
  TestAllocator alloc;
  {
    Rig r(alloc);
    r.cameras.initialize();
    FakeCameraHost *a = r.make(), *b = r.make();
    r.bay.plug(0, a);
    CameraEvent e;
    CHECK(r.cameras.poll(&e));
    SharedPointer<ICamera> first = e.camera;
    CHECK(first->start(16, 16, 30));
    // another camera takes the port before anyone looked
    r.bay.unplug(0);
    r.bay.plug(0, b);
    CHECK(r.cameras.poll(&e) && e.kind == CameraEvent::Kind::Removed && e.camera.get() == first.get() && e.was_streaming);
    CHECK(r.cameras.poll(&e) && e.kind == CameraEvent::Kind::Added && e.id != 1);
    SharedPointer<ICamera> second = e.camera;
    const size_t b_controls = b->controls.size();
    CHECK(!first->present() && !first->start(16, 16, 30));
    first->pump();
    first->stop();
    CHECK(b->controls.size() == b_controls);   // the old camera never touched the new one
    CHECK(second->start(16, 16, 30));
    send_frames(r.bay, 0, 2);
    r.cameras.pump();
    CHECK(read_grey(*second, alloc));
    first.reset();
    second.reset();
    e = CameraEvent();
    r.cameras.shutdown();
  }
  CHECK(alloc.live == 0);
}

static void test_two_cameras_and_other_devices() {
  TestAllocator alloc;
  {
    Rig r(alloc);
    r.cameras.initialize();
    FakeCameraHost *a = r.make(), *b = r.make();
    Layout bulk = small_camera();
    bulk.bulk = true;
    FakeCameraHost* unusable = r.make(bulk);   // streams over bulk: left alone
    r.bay.plug(0, a);
    r.bay.plug(1, unusable);
    r.bay.plug(3, b);
    CameraEvent e;
    CHECK(r.cameras.poll(&e) && r.cameras.poll(&e) && !r.cameras.poll(&e) && r.cameras.count() == 2);
    SharedPointer<ICamera> ca = r.cameras.share(0), cb = r.cameras.share(1);
    CHECK(ca->start(16, 16, 30) && cb->start(16, 16, 15));
    send_frames(r.bay, 0, 2);
    send_frames(r.bay, 3, 2);
    r.cameras.pump();
    CHECK(read_grey(*ca, alloc) && read_grey(*cb, alloc));
    // one leaves, the other carries on
    r.bay.unplug(0);
    CHECK(r.cameras.poll(&e) && e.kind == CameraEvent::Kind::Removed && e.was_streaming);
    CHECK(r.cameras.count() == 1 && r.cameras.at(0) == cb.get());
    send_frames(r.bay, 3, 2);
    r.cameras.pump();
    CHECK(read_grey(*cb, alloc) && !ca->present() && cb->present());
    r.bay.unplug(1);
    r.bay.unplug(3);
    CHECK(r.cameras.poll(&e) && !r.cameras.poll(&e) && r.cameras.count() == 0);
    e = CameraEvent();
    ca.reset();
    cb.reset();
    r.cameras.shutdown();
  }
  CHECK(alloc.live == 0);
}

int main() {
  test_plug_stream_unplug();
  test_camera_at_start_up();
  test_replaced_before_noticed();
  test_two_cameras_and_other_devices();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("CameraHotplugTest: all passed\n");
  return 0;
}
