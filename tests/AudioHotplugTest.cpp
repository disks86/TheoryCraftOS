// Host-side tests for USB audio hot-plug: SystemAudioDevices::poll() and the AudioRouter that keeps the mixer and the capture hub on the
// right devices, against a fake host controller with four ports into which fake USB headsets, DACs and microphones are plugged and from which
// they are pulled. Programs' streams survive every switch, the master volume goes along, and the software fallback takes over when the last
// output leaves.
#include <memory>
#include <string>
#include <vector>
#include "Audio/AudioCaptureHub.hpp"
#include "Audio/AudioMixer.hpp"
#include "Audio/AudioRouter.hpp"
#include "FakeAudioBay.hpp"

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct FakeClock2 final : IClock {
  const char* name() const override { return "fake clock"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint64_t now_us() const override { return 0; }
};

struct Notes {
  std::vector<std::string> lines;
  static void add(void* self, StringView text) { static_cast<Notes*>(self)->lines.push_back(std::string(text.data(), text.size())); }
  bool has(const char* needle) const {
    for (const std::string& s : lines) if (s.find(needle) != std::string::npos) return true;
    return false;
  }
};

// Everything one test needs: the bay, a pile of fake devices to plug in, the audio devices on top, a mixer, a hub and a router.
struct Rig {
  TestAllocator& alloc;
  FakePci pci;
  FakeClock2 clock;
  FakeAudioBay bay;
  FakeUsb usb;
  std::vector<std::unique_ptr<FakeAudioHost>> made;
  SystemAudioDevices devices{pci, clock, alloc, nullptr, &usb};
  UniquePointer<audio::AudioMixer> mixer;
  UniquePointer<audio::AudioCaptureHub> hub;
  UniquePointer<audio::AudioRouter> router;
  Notes notes;

  explicit Rig(TestAllocator& allocator) : alloc(allocator) { usb.hosts.push_back(&bay); }
  FakeAudioHost* make(bool output, bool input) {
    Layout l;
    l.output = output;
    l.input = input;
    made.push_back(std::make_unique<FakeAudioHost>(l));
    return made.back().get();
  }
  // Starts the devices, the mixer on the first one and the hub on the first microphone, and the router.
  void start() {
    devices.initialize();
    mixer = UniquePointer<audio::AudioMixer>::make(alloc, devices.share(0), alloc);
    CHECK(mixer && mixer->initialize());
    hub = UniquePointer<audio::AudioCaptureHub>::make(alloc, alloc);
    CHECK(hub && hub->initialize());
    for (uint32_t i = 0; i < devices.count(); ++i)
      if (IAudioCapture* c = devices.at(i)->capture()) { hub->set_source(c, devices.share(i)); break; }
    router = UniquePointer<audio::AudioRouter>::make(alloc, devices, mixer.get(), hub.get());
    router->set_notifier(Notes::add, &notes);
  }
  void stop() {
    router.reset();
    hub.reset();
    mixer.reset();
    devices.shutdown();
  }
  bool mixer_on_usb() { return strncmp(mixer->current_device()->name(), "USB audio", 9) == 0; }
  bool mixer_on_fallback() { return strncmp(mixer->current_device()->name(), "Software audio", 14) == 0; }
};

static Bytes tone(int frames, int16_t value) {
  Bytes b;
  for (int i = 0; i < frames; ++i) { b.push_back(static_cast<uint8_t>(value)); b.push_back(static_cast<uint8_t>(value >> 8)); b.push_back(static_cast<uint8_t>(value)); b.push_back(static_cast<uint8_t>(value >> 8)); }
  return b;
}

static bool sent_contains(FakeAudioHost* host, uint8_t endpoint, uint8_t lo, uint8_t hi) {
  for (const Bytes& p : host->endpoints[endpoint].sent)
    for (size_t i = 0; i + 1 < p.size(); ++i)
      if (p[i] == lo && p[i + 1] == hi) return true;
  return false;
}

// Plays `frames` of `value` through the mixer into whatever it is on, letting the controller drain the fake USB device as it goes.
static void play(Rig& r, SharedPointer<audio::AudioStream>& stream, FakeAudioHost* host, uint32_t port, int frames, int16_t value) {
  const Bytes pcm = tone(frames, value);
  stream->write(ConstByteView(pcm.data(), pcm.size()));
  for (int i = 0; i < 60; ++i) {
    r.mixer->pump();
    if (host) r.bay.play_out(port);
  }
}

static void test_headset_comes_and_goes() {
  TestAllocator alloc;
  {
    Rig r(alloc);
    r.start();
    CHECK(r.devices.count() == 1 && !r.devices.is_hardware(0) && r.mixer_on_fallback() && r.hub->source_device() == r.devices.at(0));   // (silence)
    CHECK(r.router->poll() == 0);
    auto stream = r.mixer->open_stream();
    auto mic = r.hub->open_stream();
    CHECK(stream && mic);
    r.mixer->set_master_volume(30);

    FakeAudioHost* headset = r.make(true, true);
    r.bay.plug(2, headset);
    CHECK(r.router->poll() == 1);
    CHECK(r.devices.count() == 2 && r.devices.is_hardware(1) && r.mixer_on_usb());
    CHECK(r.hub->source_device() == r.devices.at(1));
    CHECK(r.notes.has("Audio output: USB audio") && r.notes.has("Audio input:"));
    CHECK(r.mixer->master_volume() == 30 && r.mixer->current_device()->volume() == 30);
    // the volume reached the headset's feature unit: 30 % of the top 60 dB: 0 - 70 % * 15360 = -10752 = 0xD600
    const FakeAudioHost::Control* vol = nullptr;
    for (const FakeAudioHost::Control& c : headset->controls)
      if (c.type == 0x21 && c.request == 0x01 && c.value == 0x0200 && c.index == 0x0200) vol = &c;   // (the last one sent)
    CHECK(vol && vol->data == Bytes({0x00, 0xD6}));

    // the program's stream plays to the headset now, and the microphone is heard
    play(r, stream, headset, 2, 480, 1000);
    CHECK(sent_contains(headset, 1, 0xE8, 0x03));
    Bytes voice;
    for (int i = 0; i < 48; ++i) { voice.push_back(0x34); voice.push_back(0x12); }
    headset->endpoints[0x81].incoming.push_back(voice);
    r.hub->pump();
    uint8_t buffer[512];
    CHECK(mic->read(ByteView(buffer, sizeof buffer)) == 48 * 4 && buffer[0] == 0x34 && buffer[1] == 0x12);

    // pulled out: back to the fallback, the streams are still there and still accepted
    r.bay.unplug(2);
    CHECK(r.router->poll() == 1);
    CHECK(r.devices.count() == 1 && r.mixer_on_fallback() && r.hub->source_device() == r.devices.at(0));
    CHECK(r.mixer->stream_count() == 1 && r.hub->stream_count() == 1);
    const uint64_t before = r.mixer->current_device()->statistics().frames_queued;
    play(r, stream, nullptr, 0, 480, 2000);
    CHECK(r.mixer->current_device()->statistics().frames_queued > before);
    CHECK(r.mixer->master_volume() == 30);
    CHECK(r.router->poll() == 0);
    r.stop();
  }
  CHECK(alloc.live == 0);
}

static void test_dac_at_boot() {
  TestAllocator alloc;
  {
    Rig r(alloc);
    FakeAudioHost* dac = r.make(true, false);
    r.bay.plug(0, dac);
    r.start();   // the USB DAC is the first device, there is no fallback
    CHECK(r.devices.count() == 1 && r.mixer_on_usb() && r.devices.is_hardware(0));
    CHECK(r.router->poll() == 0);   // (nothing new: the DAC was there at start-up)
    auto stream = r.mixer->open_stream();
    play(r, stream, dac, 0, 480, 1500);
    CHECK(sent_contains(dac, 1, 0xDC, 0x05));
    r.bay.unplug(0);
    CHECK(r.router->poll() == 2);   // the DAC leaves, the software fallback is brought up
    CHECK(r.devices.count() == 1 && r.mixer_on_fallback() && !r.devices.is_hardware(0));
    CHECK(r.mixer->stream_count() == 1);
    r.stop();
  }
  CHECK(alloc.live == 0);
}

static void test_two_headsets() {
  TestAllocator alloc;
  {
    Rig r(alloc);
    r.start();
    FakeAudioHost *a = r.make(true, false), *b = r.make(true, false);
    r.bay.plug(0, a);
    CHECK(r.router->poll() == 1 && r.mixer->current_device() == r.devices.at(1));
    IAudioDevice* first = r.devices.at(1);
    r.bay.plug(1, b);
    CHECK(r.router->poll() == 1 && r.mixer->current_device() == r.devices.at(2) && r.devices.count() == 3);
    IAudioDevice* second = r.devices.at(2);
    // the one not in use leaves: the mixer stays where it is
    r.bay.unplug(0);
    CHECK(r.router->poll() == 1 && r.devices.count() == 2 && r.mixer->current_device() == second);
    (void)first;
    // both again, then the one in use leaves: the other hardware output, not the fallback
    FakeAudioHost *c = r.make(true, false), *d = r.make(true, false);
    r.bay.plug(0, c);
    CHECK(r.router->poll() == 1);
    r.bay.plug(3, d);
    CHECK(r.router->poll() == 1 && r.mixer->current_device() == r.devices.at(r.devices.count() - 1));
    r.bay.unplug(3);
    CHECK(r.router->poll() == 1);
    CHECK(r.mixer_on_usb());   // (second or c: a hardware output)
    r.bay.unplug(0);
    r.bay.unplug(1);
    CHECK(r.router->poll() == 2);
    CHECK(r.mixer_on_fallback() && r.devices.count() == 1);
    r.stop();
  }
  CHECK(alloc.live == 0);
}

static void test_microphone_only() {
  TestAllocator alloc;
  {
    Rig r(alloc);
    r.start();
    FakeAudioHost* mic = r.make(false, true);
    r.bay.plug(1, mic);
    CHECK(r.router->poll() == 1);
    CHECK(r.mixer_on_fallback());                  // a microphone plays nothing: the mixer stays
    CHECK(r.hub->source_device() == r.devices.at(1) && r.devices.count() == 2 && !r.devices.at(1)->has_output());
    auto stream = r.hub->open_stream();
    Bytes samples;
    for (int i = 0; i < 48; ++i) { samples.push_back(0x10); samples.push_back(0x00); }
    mic->endpoints[0x81].incoming.push_back(samples);
    r.hub->pump();
    uint8_t buffer[512];
    CHECK(stream->read(ByteView(buffer, sizeof buffer)) == 48 * 4);
    r.bay.unplug(1);
    CHECK(r.router->poll() == 1 && r.hub->source_device() == r.devices.at(0) && r.mixer_on_fallback() && r.devices.count() == 1);
    // and a headset's microphone after a standalone one: the hub moves to the newest
    FakeAudioHost *mic2 = r.make(false, true), *headset = r.make(true, true);
    r.bay.plug(0, mic2);
    r.bay.plug(2, headset);
    CHECK(r.router->poll() == 2 && r.mixer_on_usb() && r.hub->source_device() == r.devices.at(2));
    r.bay.unplug(2);   // the headset leaves: the hub falls back to the other microphone
    CHECK(r.router->poll() == 1 && r.mixer_on_fallback() && r.hub->source_device() == r.devices.at(1) && !r.devices.at(1)->has_output());
    r.stop();
  }
  CHECK(alloc.live == 0);
}

static void test_replaced_before_noticed() {
  TestAllocator alloc;
  {
    Rig r(alloc);
    r.start();
    FakeAudioHost *a = r.make(true, true), *b = r.make(true, true);
    r.bay.plug(1, a);
    CHECK(r.router->poll() == 1);
    const uint32_t id_a = r.devices.id(1);
    r.bay.unplug(1);
    r.bay.plug(1, b);
    CHECK(r.router->poll() == 2);   // the old one gone, a new one there
    CHECK(r.devices.count() == 2 && r.devices.id(1) != id_a && r.mixer_on_usb() && r.mixer->current_device() == r.devices.at(1));
    // the old device is not the one the mixer talks to: sound goes to b
    auto stream = r.mixer->open_stream();
    play(r, stream, b, 1, 480, 1000);
    CHECK(sent_contains(b, 1, 0xE8, 0x03));
    const size_t a_packets = a->endpoints[1].sent.size();
    play(r, stream, b, 1, 480, 1000);
    CHECK(a->endpoints[1].sent.size() == a_packets);
    r.stop();
  }
  CHECK(alloc.live == 0);
}

static void test_gone_before_the_router_looks() {
  // the mixer keeps running while a device has left and nobody has switched yet: nothing breaks, nothing is sent to a dead port
  TestAllocator alloc;
  {
    Rig r(alloc);
    r.start();
    FakeAudioHost* headset = r.make(true, true);
    r.bay.plug(0, headset);
    CHECK(r.router->poll() == 1);
    auto stream = r.mixer->open_stream();
    auto mic = r.hub->open_stream();
    r.bay.unplug(0);
    play(r, stream, nullptr, 0, 480, 1000);
    r.hub->pump();
    uint8_t buffer[64];
    CHECK(mic->read(ByteView(buffer, sizeof buffer)) == 0);
    const size_t sent = headset->endpoints[1].sent.size();
    play(r, stream, nullptr, 0, 480, 1000);
    CHECK(headset->endpoints[1].sent.size() == sent);
    CHECK(r.router->poll() == 1 && r.mixer_on_fallback());
    r.stop();
  }
  CHECK(alloc.live == 0);
}

static void test_without_mixer_or_hub() {
  TestAllocator alloc;
  {
    Rig r(alloc);
    r.devices.initialize();
    audio::AudioRouter bare(r.devices, nullptr, nullptr);
    FakeAudioHost* headset = r.make(true, true);
    r.bay.plug(0, headset);
    CHECK(bare.poll() == 1);
    r.bay.unplug(0);
    CHECK(bare.poll() == 1);
    bare.stop_service();
    bare.stop_service();
    r.devices.shutdown();
  }
  CHECK(alloc.live == 0);
}

int main() {
  test_headset_comes_and_goes();
  test_dac_at_boot();
  test_two_headsets();
  test_microphone_only();
  test_replaced_before_noticed();
  test_gone_before_the_router_looks();
  test_without_mixer_or_hub();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("AudioHotplugTest: all passed\n");
  return 0;
}
