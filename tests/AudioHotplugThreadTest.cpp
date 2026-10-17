// USB audio hot-plug with real threads, run under ThreadSanitizer: headsets, DACs and microphones are plugged in and pulled out while the mixer
// and the capture hub run on their own threads, a program keeps writing to a stream and another keeps reading the microphone, and the router
// polls. What the program reads must be silence or the microphone's samples, never anything else; no race may be reported; the sound server must
// end up on the software fallback with every stream still open; and no memory may be left.
#include <time.h>
#include <memory>
#include <string>
#include <vector>
#include "Audio/AudioCaptureHub.hpp"
#include "Audio/AudioMixer.hpp"
#include "Audio/AudioRouter.hpp"
#include "FakeAudioBay.hpp"
#include "Kernel/Thread.hpp"

static int g_failed = 0;
static void fail(int line, const char* what) { printf("FAILED line %d: %s\n", line, what); __atomic_add_fetch(&g_failed, 1, __ATOMIC_RELAXED); }
#define CHECK(...) do { if (!(__VA_ARGS__)) fail(__LINE__, #__VA_ARGS__); } while (0)

static void nap(long us) {
  timespec ts{0, us * 1000};
  nanosleep(&ts, nullptr);
}

struct FakeClock2 final : IClock {
  const char* name() const override { return "fake clock"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint64_t now_us() const override { return 0; }
};

struct World {
  TestAllocator alloc;
  FakePci pci;
  FakeClock2 clock;
  FakeAudioBay bay;
  FakeUsb usb;
  SystemAudioDevices* devices = nullptr;
  audio::AudioMixer* mixer = nullptr;
  audio::AudioCaptureHub* hub = nullptr;
  audio::AudioRouter* router = nullptr;
  SharedPointer<audio::AudioStream> stream;
  SharedPointer<audio::CaptureStream> mic;
  std::vector<FakeAudioHost*> all;     // every device ever made (plugger only)
  FakeAudioHost* in_port[FakeAudioBay::kPorts] = {};
  bool stop = false;
  long plugs = 0, unplugs = 0, written = 0, read_bytes = 0, voice_frames = 0, handled = 0;
};

static bool stopping(World* w) { return __atomic_load_n(&w->stop, __ATOMIC_ACQUIRE); }

static const uint8_t kVoiceLo = 0x34, kVoiceHi = 0x12;

static void plugger(void* arg) {
  World* w = static_cast<World*>(arg);
  uint32_t seed = 31337;
  auto rnd = [&] { seed = seed * 1664525 + 1013904223; return seed >> 8; };
  for (int i = 0; i < 500; ++i) {
    const uint32_t port = rnd() % FakeAudioBay::kPorts;
    if (w->in_port[port]) {
      w->bay.unplug(port);
      w->in_port[port] = nullptr;
      __atomic_add_fetch(&w->unplugs, 1, __ATOMIC_RELAXED);
    } else {
      const uint32_t kind = rnd() % 3;   // headset, DAC, microphone
      Layout l;
      l.output = kind != 2;
      l.input = kind != 1;
      FakeAudioHost* device = new FakeAudioHost(l);
      w->all.push_back(device);
      w->in_port[port] = device;
      w->bay.plug(port, device);
      __atomic_add_fetch(&w->plugs, 1, __ATOMIC_RELAXED);
    }
    // time passes on every port: the playback queues drain and the microphones record
    for (uint32_t p = 0; p < FakeAudioBay::kPorts; ++p) {
      if (!w->in_port[p]) continue;
      w->bay.play_out(p);
      Bytes voice;
      for (int n = 0; n < 48; ++n) { voice.push_back(kVoiceLo); voice.push_back(kVoiceHi); }
      w->bay.feed_capture(p, voice);
    }
    nap(100 + rnd() % 1200);
  }
}

static void poller(void* arg) {
  World* w = static_cast<World*>(arg);
  while (!stopping(w)) {
    const uint32_t n = w->router->poll();
    __atomic_add_fetch(&w->handled, n, __ATOMIC_RELAXED);
    nap(200);
  }
}

static void writer(void* arg) {
  World* w = static_cast<World*>(arg);
  Bytes tone;
  for (int i = 0; i < 240; ++i) { tone.push_back(0xE8); tone.push_back(0x03); tone.push_back(0xE8); tone.push_back(0x03); }
  while (!stopping(w)) {
    const uint32_t taken = w->stream->write(ConstByteView(tone.data(), tone.size()));
    __atomic_add_fetch(&w->written, taken, __ATOMIC_RELAXED);
    nap(500);
  }
}

static void reader(void* arg) {
  World* w = static_cast<World*>(arg);
  uint8_t buffer[4096];
  while (!stopping(w)) {
    const uint32_t n = w->mic->read(ByteView(buffer, sizeof buffer));
    __atomic_add_fetch(&w->read_bytes, n, __ATOMIC_RELAXED);
    for (uint32_t i = 0; i + 3 < n; i += 4) {
      const bool silence = buffer[i] == 0 && buffer[i + 1] == 0 && buffer[i + 2] == 0 && buffer[i + 3] == 0;
      const bool voice = buffer[i] == kVoiceLo && buffer[i + 1] == kVoiceHi && buffer[i + 2] == kVoiceLo && buffer[i + 3] == kVoiceHi;
      if (!silence && !voice) { fail(__LINE__, "a frame that is neither silence nor the microphone"); break; }
      if (voice) __atomic_add_fetch(&w->voice_frames, 1, __ATOMIC_RELAXED);
    }
    nap(300);
  }
}

static void test_stress() {
  World* w = new World;
  w->usb.hosts.push_back(&w->bay);
  {
    SystemAudioDevices devices(w->pci, w->clock, w->alloc, nullptr, &w->usb);
    w->devices = &devices;
    devices.initialize();
    audio::AudioMixer mixer(devices.share(0), w->alloc);
    CHECK(mixer.initialize() && mixer.start_service());
    w->mixer = &mixer;
    audio::AudioCaptureHub hub(w->alloc);
    CHECK(hub.initialize());
    hub.set_source(devices.at(0)->capture(), devices.share(0));
    CHECK(hub.start_service());
    w->hub = &hub;
    audio::AudioRouter router(devices, &mixer, &hub);
    w->router = &router;
    w->stream = mixer.open_stream();
    w->mic = hub.open_stream();
    CHECK(w->stream && w->mic);

    kernel::Thread* threads[3];
    threads[0] = kernel::spawn(poller, w, "poller");
    threads[1] = kernel::spawn(writer, w, "writer");
    threads[2] = kernel::spawn(reader, w, "reader");
    kernel::Thread* plug = kernel::spawn(plugger, w, "plugger");
    kernel::join(plug);
    for (uint32_t p = 0; p < FakeAudioBay::kPorts; ++p)
      if (w->in_port[p]) { w->bay.unplug(p); w->in_port[p] = nullptr; __atomic_add_fetch(&w->unplugs, 1, __ATOMIC_RELAXED); }
    for (int i = 0; i < 300 && devices.count() > 1; ++i) nap(2000);
    nap(30000);
    __atomic_store_n(&w->stop, true, __ATOMIC_RELEASE);
    for (int i = 0; i < 3; ++i) kernel::join(threads[i]);

    router.poll();
    CHECK(devices.count() == 1 && !devices.is_hardware(0));
    CHECK(strncmp(mixer.current_device()->name(), "Software audio", 14) == 0 && hub.source_device() == devices.at(0));
    CHECK(mixer.stream_count() == 1 && hub.stream_count() == 1);   // the program's streams survived every switch
    CHECK(w->plugs > 100 && w->unplugs == w->plugs);
    CHECK(w->written > 100000 && w->read_bytes > 1000);
    printf("  %ld plugs, %ld changes routed, %ld bytes written, %ld bytes read, %ld voice frames\n", w->plugs, w->handled, w->written, w->read_bytes, w->voice_frames);
    w->stream.reset();
    w->mic.reset();
    hub.stop_service();
    mixer.stop_service();
    w->router = nullptr;
    devices.shutdown();
  }
  CHECK(w->alloc.live == 0);
  for (FakeAudioHost* d : w->all) delete d;
  delete w;
}

// The router's own service thread.
static void test_service() {
  TestAllocator alloc;
  FakePci pci;
  FakeClock2 clock;
  FakeAudioBay bay;
  FakeUsb usb;
  usb.hosts.push_back(&bay);
  Layout l;
  FakeAudioHost headset(l);
  {
    SystemAudioDevices devices(pci, clock, alloc, nullptr, &usb);
    devices.initialize();
    audio::AudioMixer mixer(devices.share(0), alloc);
    CHECK(mixer.initialize());
    audio::AudioRouter router(devices, &mixer, nullptr);
    CHECK(router.start_service() && router.start_service());
    bay.plug(1, &headset);
    bool seen = false;
    for (int i = 0; i < 60 && !seen; ++i) {
      nap(100000);
      SharedPointer<IAudioDevice> d = mixer.shared_device();
      seen = d && strncmp(d->name(), "USB audio", 9) == 0;
    }
    CHECK(seen);
    bay.unplug(1);
    bool gone = false;
    for (int i = 0; i < 60 && !gone; ++i) {
      nap(100000);
      SharedPointer<IAudioDevice> d = mixer.shared_device();
      gone = d && strncmp(d->name(), "Software audio", 14) == 0;
    }
    CHECK(gone);
    router.stop_service();
    router.stop_service();
    devices.shutdown();
  }
  CHECK(alloc.live == 0);
}

int main() {
  test_stress();
  test_service();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("AudioHotplugThreadTest: all passed\n");
  return 0;
}
