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

// Host-side tests for USB MIDI: finding the MIDIStreaming interface of keyboards, interfaces and synthesizers in their descriptors, the 4-byte event
// packets, the device (batched output, input without waiting, unplugging), and the service that routes a keyboard's keys to a synthesizer, the
// fallback output or the live software synthesizer, plays songs on the USB hardware, and follows devices plugged in and out.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <deque>
#include <vector>
#include "Audio/AudioMixer.hpp"
#include "Hardware/AudioDevice/IAudioDevice.hpp"
#include "Hardware/MidiDevice/Usb/UsbMidiDescriptors.hpp"
#include "Hardware/MidiDevice/Usb/UsbMidiDevice.hpp"
#include "Hardware/MidiDevice/Usb/UsbMidiService.hpp"
#include "Media/ByteSource.hpp"
#include "Midi/MidiStream.hpp"

using namespace hardware;

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { if (!size) return nullptr; ++live; return aligned_alloc(64, (size + 63) / 64 * 64); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    if (!size) return nullptr;
    ++live;
    if (alignment < 64) alignment = 64;
    return aligned_alloc(alignment, (size + alignment - 1) / alignment * alignment);
  }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

typedef std::vector<uint8_t> Bytes;

// --- Descriptors ------------------------------------------------------------------------------------

struct Config {
  Bytes b;
  void add(std::initializer_list<uint8_t> d) {
    b.push_back(static_cast<uint8_t>(d.size() + 1));
    b.insert(b.end(), d.begin(), d.end());
  }
};

// A class-compliant device: configuration, an audio control interface, and the MIDIStreaming interface with bulk OUT 0x02 and IN 0x81.
static Bytes midi_config(bool out, bool in, bool synth, uint8_t alternate_with_endpoints = 0, uint8_t subclass = 3) {
  Config c;
  c.add({2, 0, 0, 2, 1, 0, 0x80, 50});   // placeholder total length, patched below
  c.add({4, 0, 0, 0, 1, 1, 0, 0});       // interface 0: audio control, no endpoints
  c.add({0x24, 1, 0, 1, 9, 0, 1, 1});    // AC header
  const uint8_t endpoints = (out ? 1 : 0) + (in ? 1 : 0);
  if (alternate_with_endpoints) c.add({4, 1, 0, 0, 1, subclass, 0, 0});   // alternate 0 without endpoints
  c.add({4, 1, alternate_with_endpoints, endpoints, 1, subclass, 0, 0});
  c.add({0x24, 1, 0, 1, 0x25, 0});       // MS header
  c.add({0x24, 2, 1, 1, 0});             // MIDI IN jack
  c.add({0x24, 3, 1, 2, 1, 1, 1, 0});    // MIDI OUT jack
  if (synth) c.add({0x24, 4, 3, 1, 1, 0, 0, 0, 0, 0});   // an element: a sound maker
  if (out) {
    c.add({5, 0x02, 2, 64, 0, 0, 0, 0});
    c.add({0x25, 1, 1, 1});
  }
  if (in) {
    c.add({5, 0x81, 2, 64, 0, 0, 0, 0});
    c.add({0x25, 1, 1, 2});
  }
  c.b[2] = static_cast<uint8_t>(c.b.size());
  return c.b;
}

static void test_descriptors() {
  UsbMidiModel m;
  CHECK(parse_usb_midi_configuration(ConstByteView(midi_config(true, true, false).data(), midi_config(true, true, false).size()), &m));
  CHECK(m.configuration == 1 && m.interface_number == 1 && m.alternate == 0 && m.out_endpoint == 0x02 && m.in_endpoint == 0x81);
  CHECK(m.out_max_packet == 64 && m.in_max_packet == 64 && !m.has_synth && m.out_cables == 1 && m.in_cables == 1);
  Bytes synth = midi_config(true, true, true);
  CHECK(parse_usb_midi_configuration(ConstByteView(synth.data(), synth.size()), &m) && m.has_synth);
  Bytes keys_only = midi_config(false, true, false);
  CHECK(parse_usb_midi_configuration(ConstByteView(keys_only.data(), keys_only.size()), &m) && m.out_endpoint == 0 && m.in_endpoint == 0x81 && m.valid());
  Bytes out_only = midi_config(true, false, true);
  CHECK(parse_usb_midi_configuration(ConstByteView(out_only.data(), out_only.size()), &m) && m.in_endpoint == 0 && m.out_endpoint == 0x02);
  Bytes alt = midi_config(true, true, false, 1);
  CHECK(parse_usb_midi_configuration(ConstByteView(alt.data(), alt.size()), &m) && m.alternate == 1 && m.out_endpoint == 0x02);
  Bytes streaming = midi_config(true, true, false, 0, 2);   // audio streaming, not MIDI
  CHECK(!parse_usb_midi_configuration(ConstByteView(streaming.data(), streaming.size()), &m));
  Bytes none = midi_config(false, false, false);
  CHECK(!parse_usb_midi_configuration(ConstByteView(none.data(), none.size()), &m));
  CHECK(!parse_usb_midi_configuration(ConstByteView(), &m));
  Bytes cut = midi_config(true, true, true);
  for (size_t n = 0; n < cut.size(); n += 3) parse_usb_midi_configuration(ConstByteView(cut.data(), n), &m);   // (nothing read outside)
  // Several cables on the endpoint are noted.
  Bytes cables = midi_config(true, true, false);
  for (size_t i = 0; i + 3 < cables.size(); ++i)
    if (cables[i] == 5 && cables[i + 1] == 0x25 && cables[i + 2] == 1 && cables[i + 3] == 1) { cables[i + 3] = 3; break; }
  CHECK(parse_usb_midi_configuration(ConstByteView(cables.data(), cables.size()), &m) && m.out_cables == 3);
}

struct Collected {
  struct Message { uint8_t status, d1, d2; };
  std::vector<Message> messages;
  static void fn(void* c, uint8_t s, uint8_t a, uint8_t b) { static_cast<Collected*>(c)->messages.push_back({s, a, b}); }
};

static void test_packets() {
  uint8_t p[4];
  CHECK(encode_usb_midi_packet(0, 0x91, 60, 100, p) && p[0] == 0x09 && p[1] == 0x91 && p[2] == 60 && p[3] == 100);
  CHECK(encode_usb_midi_packet(2, 0xC3, 5, 99, p) && p[0] == 0x2C && p[1] == 0xC3 && p[2] == 5 && p[3] == 0);   // program change: two bytes
  CHECK(encode_usb_midi_packet(0, 0xE0, 0xFF, 0xFF, p) && p[2] == 0x7F && p[3] == 0x7F);
  CHECK(!encode_usb_midi_packet(0, 0xF8, 0, 0, p) && !encode_usb_midi_packet(0, 0x40, 0, 0, p));
  const uint8_t data[] = {0x09, 0x90, 64, 80,        // note on
                          0x04, 0xF0, 0x7E, 0x7F,    // system exclusive: skipped
                          0x0F, 0xF8, 0, 0,          // clock: skipped
                          0x1B, 0xB1, 7, 90,         // controller on cable 1
                          0x08, 0x90, 64, 0,         // header says note off, status says note on: damaged, skipped
                          0x00, 0x00, 0, 0,          // empty packet
                          0x0E, 0xEF, 0, 0x40, 0x09, 0x90, 1};   // pitch bend, then a partial packet
  Collected got;
  CHECK(decode_usb_midi_packets(ConstByteView(data, sizeof data), &Collected::fn, &got) == 3);
  CHECK(got.messages.size() == 3 && got.messages[0].status == 0x90 && got.messages[0].d1 == 64 && got.messages[0].d2 == 80);
  CHECK(got.messages[1].status == 0xB1 && got.messages[1].d2 == 90 && got.messages[2].status == 0xEF && got.messages[2].d2 == 0x40);
}

// --- A fake host controller ---------------------------------------------------------------------------

struct FakeHost final : IUsbHostController {
  Bytes config;
  bool present = true;
  uint32_t generation = 1;
  bool in_streams = true, bulk_out_fails = false;
  std::deque<Bytes> in_queue;           // what the device "sends"
  std::vector<Bytes> out_transfers;     // what the host gave the bulk OUT endpoint
  int configurations = 0, interfaces_set = 0, endpoints_started = 0, stream_starts = 0, resets = 0;
  uint8_t configured = 0;

  const char* name() const override { return "fake host"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  UsbControllerKind kind() const override { return UsbControllerKind::Xhci; }
  uint32_t port_count() const override { return 1; }
  bool port_info(uint32_t, UsbPortInfo* out) const override { out->connected = present; out->speed = UsbSpeed::Full; return true; }
  bool device_descriptor(uint32_t, UsbDeviceDescriptor* out) const override {
    if (!present) return false;
    memset(out, 0, sizeof *out);
    out->length = 18;
    out->descriptor_type = 1;
    out->vendor_id = 0x1234;
    out->product_id = 0xABCD;
    out->product_string = 2;
    out->configuration_count = 1;
    return true;
  }
  bool configuration_descriptor(uint32_t, ByteView buffer, uint16_t* length) const override {
    if (!present || config.size() > buffer.size()) return false;
    for (size_t i = 0; i < config.size(); ++i) buffer[i] = config[i];
    *length = static_cast<uint16_t>(config.size());
    return true;
  }
  bool set_configuration(uint32_t, uint8_t value) override { ++configurations; configured = value; return present; }
  bool control_out(uint32_t, uint8_t type, uint8_t request, uint16_t, uint16_t) override {
    if (type == 0x01 && request == 11) ++interfaces_set;
    return present;
  }
  bool control_in(uint32_t, uint8_t type, uint8_t request, uint16_t value, uint16_t, ByteView buffer) override {
    if (type == 0x80 && request == 6 && value == ((3u << 8) | 2)) {   // the product string: "Keys"
      const char* s = "Keys";
      buffer[0] = 10;
      buffer[1] = 3;
      for (int i = 0; i < 4; ++i) { buffer[2 + 2 * i] = static_cast<uint8_t>(s[i]); buffer[3 + 2 * i] = 0; }
      return true;
    }
    return false;
  }
  bool start_interrupt_in(uint32_t, uint8_t, uint16_t, uint8_t) override { return false; }
  bool poll_interrupt_in(uint32_t, uint8_t endpoint, ByteView buffer, uint16_t* length) override {
    if (!present || endpoint != 0x81 || in_queue.empty()) return false;
    const Bytes b = in_queue.front();
    in_queue.pop_front();
    for (size_t i = 0; i < b.size() && i < buffer.size(); ++i) buffer[i] = b[i];
    *length = static_cast<uint16_t>(b.size());
    return true;
  }
  bool start_bulk_in_stream(uint32_t, uint8_t endpoint, uint16_t) override { ++stream_starts; return in_streams && endpoint == 0x81; }
  bool start_bulk_endpoint(uint32_t, uint8_t, uint16_t) override { ++endpoints_started; return true; }
  bool bulk_transfer(uint32_t, uint8_t endpoint, ByteView data, uint32_t* transferred) override {
    if (!present || bulk_out_fails || endpoint != 0x02) return false;
    out_transfers.push_back(Bytes(data.data(), data.data() + data.size()));
    if (transferred) *transferred = static_cast<uint32_t>(data.size());
    return true;
  }
  bool reset_bulk_endpoint(uint32_t, uint8_t) override { ++resets; return true; }
  uint32_t port_generation(uint32_t) const override { return generation; }

  void plug(const Bytes& c) { config = c; present = true; ++generation; }
  void unplug() { present = false; ++generation; }
  void key(std::initializer_list<uint8_t> packet) { in_queue.push_back(Bytes(packet)); }
};

struct FakeBus final : IUsbBus {
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

static void test_device() {
  TestAllocator a;
  {
    FakeHost host;
    host.config = midi_config(true, true, false);
    UniquePointer<UsbMidiDevice> keyboard = UsbMidiDevice::open(host, 0, a);
    CHECK(keyboard && host.configured == 1 && host.endpoints_started == 1 && host.stream_starts == 1 && host.interfaces_set == 0);
    CHECK(keyboard->has_input() && keyboard->has_output() && !keyboard->is_synthesizer() && !keyboard->available());
    CHECK(strcmp(keyboard->name(), "USB MIDI: Keys") == 0);

    // Keys arrive without waiting: several packets in one transfer, several transfers.
    host.key({0x09, 0x90, 60, 100, 0x09, 0x90, 64, 90});
    host.key({0x08, 0x80, 60, 0});
    Collected got;
    CHECK(keyboard->poll_input(&Collected::fn, &got) == 3 && got.messages.size() == 3 && got.messages[1].d1 == 64 && got.messages[2].status == 0x80);
    CHECK(keyboard->poll_input(&Collected::fn, &got) == 0);   // nothing: returns at once

    // Output is batched and goes out on flush, or when the batch is full.
    keyboard->send(0x90, 60, 100);
    keyboard->send(0xC0, 5, 0);
    CHECK(host.out_transfers.empty());
    keyboard->flush();
    CHECK(host.out_transfers.size() == 1 && host.out_transfers[0].size() == 8 && host.out_transfers[0][0] == 0x09 && host.out_transfers[0][4] == 0x0C);
    keyboard->flush();   // (nothing new)
    CHECK(host.out_transfers.size() == 1);
    for (int i = 0; i < 40; ++i) keyboard->send(0x90, static_cast<uint8_t>(i), 100);
    CHECK(host.out_transfers.size() == 3 && host.out_transfers[1].size() == 64 && host.out_transfers[2].size() == 64);   // two full batches went by themselves
    keyboard->flush();
    CHECK(host.out_transfers.size() == 4 && host.out_transfers[3].size() == 32);

    // A failing transfer does not hang or crash: the endpoint is brought back.
    host.bulk_out_fails = true;
    keyboard->send(0x90, 1, 1);
    keyboard->flush();
    CHECK(host.resets == 1);
    host.bulk_out_fails = false;

    // Unplugged: nothing is sent to it, and it says so.
    CHECK(keyboard->present());
    host.unplug();
    CHECK(!keyboard->present() && !keyboard->available());
    const size_t before = host.out_transfers.size();
    keyboard->send(0x90, 1, 1);
    keyboard->flush();
    CHECK(host.out_transfers.size() == before);
    keyboard.reset();
    CHECK(host.out_transfers.size() == before);   // (and closing it sends nothing either)

    // A synthesizer is an output that songs can be played on; a controller with an alternate setting gets SET_INTERFACE.
    host.plug(midi_config(true, true, true));
    UniquePointer<UsbMidiDevice> synth = UsbMidiDevice::open(host, 0, a);
    CHECK(synth && synth->is_synthesizer() && synth->available());
    synth->send(0x90, 60, 100);
    synth.reset();   // closing a device that is still there silences it
    CHECK(host.out_transfers.size() > before);
    size_t notes_off = 0;
    for (size_t i = before; i < host.out_transfers.size(); ++i)
      for (size_t k = 0; k + 3 < host.out_transfers[i].size(); k += 4) notes_off += host.out_transfers[i][k + 2] == 123;
    CHECK(notes_off == 16);
    host.plug(midi_config(true, true, false, 1));
    UniquePointer<UsbMidiDevice> alt = UsbMidiDevice::open(host, 0, a);
    CHECK(alt && host.interfaces_set == 1);
    alt.reset();

    // On a controller that cannot keep an IN endpoint armed, the device is still an output.
    host.in_streams = false;
    host.plug(midi_config(true, true, true));
    UniquePointer<UsbMidiDevice> no_input = UsbMidiDevice::open(host, 0, a);
    CHECK(no_input && !no_input->has_input() && no_input->has_output() && no_input->poll_input(&Collected::fn, &got) == 0);
    no_input.reset();
    // Something that is not MIDI is not taken.
    host.plug(midi_config(true, true, false, 0, 2));
    CHECK(!UsbMidiDevice::open(host, 0, a));
    host.unplug();
    CHECK(!UsbMidiDevice::open(host, 0, a));
  }
  CHECK(a.live == 0);
}

// --- The service --------------------------------------------------------------------------------------

struct FakeSoundDevice final : hardware::IAudioDevice {
  uint32_t queued = 0;
  uint64_t total = 0;
  const char* name() const override { return "fake audio"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint32_t write(ConstByteView pcm) override {
    uint32_t n = pcm.size() < 16384 - queued ? static_cast<uint32_t>(pcm.size()) : 16384 - queued;
    n -= n % hardware::kAudioFrameSize;
    queued += n;
    total += n;
    return n;
  }
  uint32_t queued_bytes() override { return queued; }
  uint32_t free_bytes() override { return 16384 - queued; }
  void set_volume(uint8_t) override {}
  uint8_t volume() const override { return 100; }
  void set_muted(bool) override {}
  bool muted() const override { return false; }
  hardware::AudioStatistics statistics() override { return {}; }
};

struct FakeOutput final : midi::IMidiOutput {
  bool on = true;
  std::vector<Collected::Message> got;
  int flushes = 0;
  bool available() const override { return on; }
  void send(uint8_t s, uint8_t a, uint8_t b) override { got.push_back({s, a, b}); }
  void reset() override {}
  void flush() override { ++flushes; }
};

static int notes_in(const std::vector<Bytes>& transfers, uint8_t status) {
  int n = 0;
  for (const Bytes& t : transfers)
    for (size_t k = 0; k + 3 < t.size(); k += 4) n += (t[k + 1] & 0xF0) == status;
  return n;
}

static void test_service() {
  TestAllocator a;
  {
    FakeHost host;
    FakeBus bus;
    bus.hosts.push_back(&host);
    FakeSoundDevice sound;
    audio::AudioMixer mixer(sound, a);
    CHECK(mixer.initialize());

    // A keyboard (no sound of its own) plugged in at start-up, with a mixer: the keys are played by the live software synthesizer.
    host.plug(midi_config(true, true, false));
    {
      UsbMidiService service(&bus, &mixer, a);
      CHECK(service.start() && service.device_count() == 1 && service.devices_added() == 1);
      UsbMidiService::DeviceInfo info;
      CHECK(service.device_info(0, &info) && strcmp(info.name, "USB MIDI: Keys") == 0 && info.input && info.output && !info.synthesizer);
      CHECK(!service.device_info(1, &info) && !service.available());
      CHECK(service.live_synth_running());
      host.key({0x09, 0x90, 69, 110});
      service.step(false);
      CHECK(service.messages_routed() == 1 && service.live_synth()->synth().active_voices() == 1 && host.out_transfers.empty());
      for (int i = 0; i < 10; ++i) {
        service.step(false);   // (feeds the live player when no thread does)
        mixer.pump();
        sound.queued = 0;
      }
      CHECK(sound.total > 4096);
      host.key({0x08, 0x80, 69, 0});
      service.step(false);
      CHECK(service.messages_routed() == 2);
      // Songs are not played on a keyboard's output.
      service.send(0x90, 60, 100);
      service.flush();
      CHECK(host.out_transfers.empty());
    }

    // With a fallback output (the MPU-401), the keys go there instead, and songs can be played on it.
    {
      FakeOutput mpu;
      UsbMidiService service(&bus, &mixer, a);
      service.set_fallback_output(&mpu);
      CHECK(service.start() && service.available());
      host.key({0x09, 0x90, 60, 100});
      service.step(false);
      CHECK(mpu.got.size() == 1 && mpu.got[0].status == 0x90 && service.live_synth()->synth().active_voices() == 0);
      service.send(0xC0, 7, 0);
      CHECK(mpu.got.size() == 2);
      mpu.on = false;
      CHECK(!service.available());
      host.key({0x09, 0x90, 61, 100});
      service.step(false);
      CHECK(mpu.got.size() == 2 && service.live_synth()->synth().active_voices() == 1);
    }

    // A synthesizer module (output and keys): songs are played on it, and its own keys go to it too.
    host.plug(midi_config(true, true, true));
    {
      UsbMidiService service(&bus, nullptr, a);   // (no mixer: no live synthesizer at all)
      CHECK(service.start() && service.available() && !service.live_synth_running() && service.live_synth() == nullptr);
      UsbMidiService::DeviceInfo info;
      CHECK(service.device_info(0, &info) && info.synthesizer);
      service.send(0x90, 60, 100);
      service.send(0x80, 60, 0);
      service.flush();
      CHECK(notes_in(host.out_transfers, 0x90) >= 1 && notes_in(host.out_transfers, 0x80) >= 1);
      // A song in the media player's stream goes there.
      static const uint8_t kSong[] = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0x01, 0xE0, 'M', 'T', 'r', 'k', 0, 0, 0, 13,
                                      0x00, 0x90, 0x3C, 0x64, 0x83, 0x60, 0x80, 0x3C, 0x00, 0x00, 0xFF, 0x2F, 0x00};
      media::MemoryByteSource bytes{ConstByteView(kSong, sizeof kSong)};
      host.out_transfers.clear();
      {
        UniquePointer<audio::IPcmSource> song = midi::open_midi_stream(bytes, a, &service);
        CHECK(song);
        std::vector<int16_t> pcm(48000 * 2);
        CHECK(song->read(0, ByteView(reinterpret_cast<uint8_t*>(pcm.data()), 48000 * 4)) == 48000 * 4);
        CHECK(notes_in(host.out_transfers, 0x90) == 1 && notes_in(host.out_transfers, 0x80) == 1);
      }
      CHECK(notes_in(host.out_transfers, 0xB0) >= 16);   // closing the song silenced the module
      // Keys played on the module go to the module.
      host.out_transfers.clear();
      host.key({0x09, 0x90, 72, 100});
      service.step(false);
      CHECK(notes_in(host.out_transfers, 0x90) == 1);
    }

    // Hot-plug: nothing at the start; a keyboard plugged in is found, one pulled out is dropped.
    host.unplug();
    {
      UsbMidiService service(&bus, &mixer, a);
      CHECK(service.start() && service.device_count() == 0 && !service.live_synth_running());
      service.step(true);
      CHECK(service.device_count() == 0);
      host.plug(midi_config(true, true, false));
      service.step(true);
      CHECK(service.device_count() == 1 && service.devices_added() == 1 && service.live_synth_running());
      host.key({0x09, 0x90, 64, 100});
      service.step(false);
      CHECK(service.live_synth()->synth().active_voices() == 1);
      host.unplug();
      service.step(true);
      CHECK(service.device_count() == 0 && service.devices_removed() == 1 && !service.live_synth_running());
      // The same port gets another device: a new generation, taken over again.
      host.plug(midi_config(true, true, true));
      service.step(true);
      CHECK(service.device_count() == 1 && service.devices_added() == 2 && service.available());
      // A host that cannot arm an IN endpoint: the device is there as an output only.
      host.unplug();
      service.step(true);
      host.in_streams = false;
      host.plug(midi_config(true, true, true));
      service.step(true);
      UsbMidiService::DeviceInfo info;
      CHECK(service.device_count() == 1 && service.device_info(0, &info) && !info.input && info.output && !service.live_synth_running());
    }
    // No bus at all.
    {
      UsbMidiService service(nullptr, nullptr, a);
      CHECK(service.start() && service.device_count() == 0 && !service.available());
      service.step(true);
      service.send(0x90, 1, 1);
    }
  }
  CHECK(a.live == 0);
}

int main() {
  test_descriptors();
  test_packets();
  test_device();
  test_service();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("usb midi tests passed\n");
  return 0;
}
