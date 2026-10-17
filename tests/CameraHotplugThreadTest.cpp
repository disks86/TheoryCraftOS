// USB camera hot-plug with real threads, run under ThreadSanitizer: cameras are plugged in and pulled out (streaming or not) while the pump and
// the hot-plug check run on their own threads and two programs keep starting cameras and reading pictures. Every picture a program gets must be
// the grey one the cameras send; no race may be reported; every camera must be accounted for; no memory may be left.
#include <time.h>
#include <memory>
#include <string>
#include <vector>
#include "FakeCameraBay.hpp"
#include "Kernel/Thread.hpp"

static int g_failed = 0;
static void fail(int line, const char* what) { printf("FAILED line %d: %s\n", line, what); __atomic_add_fetch(&g_failed, 1, __ATOMIC_RELAXED); }
#define CHECK(...) do { if (!(__VA_ARGS__)) fail(__LINE__, #__VA_ARGS__); } while (0)

static void nap(long us) {
  timespec ts{0, us * 1000};
  nanosleep(&ts, nullptr);
}

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

struct World {
  TestAllocator alloc;
  FakeCameraBay bay;
  FakeUsb usb;
  SystemCameras* cameras = nullptr;
  std::vector<FakeCameraHost*> all;   // every camera ever made (plugger only)
  FakeCameraHost* in_port[FakeCameraBay::kPorts] = {};
  bool stop = false;
  long plugs = 0, unplugs = 0, added = 0, removed = 0, streaming_lost = 0, started = 0, pictures = 0;
};

static bool stopping(World* w) { return __atomic_load_n(&w->stop, __ATOMIC_ACQUIRE); }

static void plugger(void* arg) {
  World* w = static_cast<World*>(arg);
  const Bytes jpeg = grey_jpeg();
  uint32_t seed = 2024;
  auto rnd = [&] { seed = seed * 1664525 + 1013904223; return seed >> 8; };
  uint8_t fid[FakeCameraBay::kPorts] = {};
  for (int i = 0; i < 500; ++i) {
    const uint32_t port = rnd() % FakeCameraBay::kPorts;
    if (w->in_port[port]) {
      w->bay.unplug(port);
      w->in_port[port] = nullptr;
      __atomic_add_fetch(&w->unplugs, 1, __ATOMIC_RELAXED);
    } else {
      Layout l;
      l.formats.clear();
      l.formats.push_back({nullptr, {{16, 16, {333333}}}});
      FakeCameraHost* cam = new FakeCameraHost(l);
      cam->max_frame = 40000;
      w->all.push_back(cam);
      w->in_port[port] = cam;
      fid[port] = 0;
      w->bay.plug(port, cam);
      __atomic_add_fetch(&w->plugs, 1, __ATOMIC_RELAXED);
    }
    // the cameras that are there send pictures
    for (uint32_t p = 0; p < FakeCameraBay::kPorts; ++p) {
      if (!w->in_port[p]) continue;
      for (int n = 0; n < 2; ++n) {
        w->bay.send(p, packetize(jpeg, fid[p], 100));
        fid[p] ^= 1;
      }
    }
    nap(100 + rnd() % 1500);
  }
}

static void poller(void* arg) {
  World* w = static_cast<World*>(arg);
  while (!stopping(w)) {
    CameraEvent e;
    while (w->cameras->poll(&e)) {
      if (e.kind == CameraEvent::Kind::Added) {
        __atomic_add_fetch(&w->added, 1, __ATOMIC_RELAXED);
      } else {
        __atomic_add_fetch(&w->removed, 1, __ATOMIC_RELAXED);
        if (e.was_streaming) __atomic_add_fetch(&w->streaming_lost, 1, __ATOMIC_RELAXED);
        if (e.camera->present() || e.camera->streaming()) fail(__LINE__, "a removed camera is still there");
      }
    }
    w->cameras->pump();
    nap(300);
  }
}

// A program: picks a camera, starts it if it is not streaming, reads pictures, now and then stops it.
static void program(void* arg) {
  World* w = static_cast<World*>(arg);
  uint32_t seed = 11 + reinterpret_cast<uintptr_t>(arg) % 13;
  auto rnd = [&] { seed = seed * 1664525 + 1013904223; return seed >> 8; };
  VideoFormat f;
  f.width = 16; f.height = 16;
  VideoFrameBuffer out(w->alloc);
  if (!out.allocate(f)) { fail(__LINE__, "buffer"); return; }
  while (!stopping(w)) {
    const uint32_t n = w->cameras->count();
    SharedPointer<ICamera> cam = n ? w->cameras->share(rnd() % n) : SharedPointer<ICamera>();
    if (cam) {
      if (!cam->streaming() && cam->start(16, 16, 30)) __atomic_add_fetch(&w->started, 1, __ATOMIC_RELAXED);
      if (cam->read_frame(out)) {
        const uint32_t v = reinterpret_cast<uint32_t*>(out.plane(0).data())[40];
        for (int s = 0; s <= 16; s += 8) {
          const int c = (v >> s) & 0xFF;
          if (c < 124 || c > 132) { fail(__LINE__, "a picture that is not grey"); break; }
        }
        __atomic_add_fetch(&w->pictures, 1, __ATOMIC_RELAXED);
      }
      (void)cam->statistics();
      (void)cam->frame_format();
      if (rnd() % 8 == 0) cam->stop();
    }
    nap(150);
  }
}

static void test_stress() {
  World* w = new World;
  w->usb.hosts.push_back(&w->bay);
  {
    SystemCameras cameras(w->usb, w->alloc);
    cameras.initialize();
    w->cameras = &cameras;
    kernel::Thread* threads[3];
    threads[0] = kernel::spawn(poller, w, "poller");
    threads[1] = kernel::spawn(program, w, "program1");
    threads[2] = kernel::spawn(program, w, "program2");
    kernel::Thread* plug = kernel::spawn(plugger, w, "plugger");
    kernel::join(plug);
    for (uint32_t p = 0; p < FakeCameraBay::kPorts; ++p)
      if (w->in_port[p]) { w->bay.unplug(p); w->in_port[p] = nullptr; __atomic_add_fetch(&w->unplugs, 1, __ATOMIC_RELAXED); }
    for (int i = 0; i < 300 && cameras.count() > 0; ++i) nap(2000);
    __atomic_store_n(&w->stop, true, __ATOMIC_RELEASE);
    for (int i = 0; i < 3; ++i) kernel::join(threads[i]);
    CameraEvent e;
    while (cameras.poll(&e)) {
      if (e.kind == CameraEvent::Kind::Added) ++w->added;
      else ++w->removed;
    }
    e = CameraEvent();
    CHECK(cameras.count() == 0);
    CHECK(w->added == w->removed && w->added > 50 && w->added <= w->plugs);
    CHECK(w->pictures > 20 && w->started > 20);
    printf("  %ld plugs, %ld cameras taken up, %ld removed while streaming, %ld streams started, %ld pictures read\n", w->plugs, w->added, w->streaming_lost, w->started, w->pictures);
    w->cameras = nullptr;
    cameras.shutdown();
  }
  CHECK(w->alloc.live == 0);
  for (FakeCameraHost* c : w->all) delete c;
  delete w;
}

struct Notes {
  std::mutex m;
  std::vector<std::string> lines;
  static void add(void* self, StringView text) {
    Notes* n = static_cast<Notes*>(self);
    std::lock_guard<std::mutex> g(n->m);
    n->lines.push_back(std::string(text.data(), text.size()));
  }
  bool has(const char* needle) {
    std::lock_guard<std::mutex> g(m);
    for (const std::string& s : lines) if (s.find(needle) != std::string::npos) return true;
    return false;
  }
};

// The service thread: it finds the camera, pumps its stream, and notices when it leaves.
static void test_service() {
  TestAllocator alloc;
  FakeCameraBay bay;
  FakeUsb usb;
  usb.hosts.push_back(&bay);
  Layout l;
  l.formats.clear();
  l.formats.push_back({nullptr, {{16, 16, {333333}}}});
  FakeCameraHost cam(l);
  cam.max_frame = 40000;
  Notes notes;
  {
    SystemCameras cameras(usb, alloc);
    cameras.initialize();
    CHECK(cameras.start_service(Notes::add, &notes) && cameras.start_service());
    bay.plug(1, &cam);
    bool seen = false;
    for (int i = 0; i < 60 && !seen; ++i) {
      nap(100000);
      seen = cameras.count() == 1;
    }
    CHECK(seen && notes.has("added, 1 modes") || notes.has("added"));
    SharedPointer<ICamera> c = cameras.share(0);
    CHECK(c && c->start(16, 16, 30));
    const Bytes jpeg = grey_jpeg();
    bay.send(1, packetize(Bytes(20, 0), 1, 100, false));
    bay.send(1, packetize(jpeg, 0, 100));
    bay.send(1, packetize(jpeg, 1, 100));
    bool got = false;
    VideoFormat f;
    f.width = 16; f.height = 16;
    VideoFrameBuffer out(alloc);
    CHECK(out.allocate(f));
    for (int i = 0; i < 60 && !got; ++i) {   // (the service pumps the packets in)
      nap(20000);
      got = c->read_frame(out);
    }
    CHECK(got);
    bay.unplug(1);
    bool gone = false;
    for (int i = 0; i < 60 && !gone; ++i) {
      nap(100000);
      gone = cameras.count() == 0;
    }
    CHECK(gone && notes.has("removed while streaming") && !c->present());
    c.reset();
    cameras.stop_service();
    cameras.stop_service();
    cameras.shutdown();
  }
  CHECK(alloc.live == 0);
}

int main() {
  test_stress();
  test_service();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("CameraHotplugThreadTest: all passed\n");
  return 0;
}
