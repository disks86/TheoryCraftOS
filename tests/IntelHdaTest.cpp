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


// IntelHdaAudioDevice against a simulated controller with a graphics card's HDMI / DisplayPort codec (tests/FakeHda.hpp, written from the High Definition Audio specification): the
// reset and command ring handshakes, which pin is chosen (a line out first, then a digital pin with a display and a valid ELD, then with a display, then any), what is set on a digital
// pin and its converter (digital output, channel count, stream, format, the audio InfoFrame in the DIP buffer), the stream descriptor and its buffer list, playback through the ring,
// retarget() when displays come and go, the failures, and shutdown.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "Hardware/AudioDevice/Intel/IntelHdaAudioDevice.hpp"
#include "FakeHda.hpp"

using namespace hardware;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct TestAllocator final : allocators::IAllocator {
  void* allocate(uint64_t size) override { return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    const uint64_t al = alignment < 16 ? 16 : alignment;
    return aligned_alloc(al, (size + al - 1) / al * al);
  }
  void deallocate(void* p) override { if (p) free(p); }
};

static constexpr uint32_t kPd = 1u << 31, kEldv = 1u << 30;

static uint32_t count_verbs(const FakeHda& h, uint32_t nid, uint32_t command) {
  uint32_t n = 0;
  for (const FakeHda::Verb& v : h.log)
    if (v.nid == nid && v.command == command) ++n;
  return n;
}

static void test_digital_pin_without_display() {
  TestAllocator alloc;
  FakeHda hda;
  IntelHdaAudioDevice dev(hda, alloc);
  CHECK(dev.initialize());
  // no display anywhere: the first digital pin (node 3) and the first converter (node 2), ready for when one comes
  CHECK(dev.output_pin() == 3 && dev.output_converter() == 2 && dev.output_is_digital() && !dev.output_has_display());
  CHECK(hda.pins[0].control == 0x40 && hda.pins[0].power == 0 && hda.converters[0].power == 0);
  CHECK(hda.converters[0].digital == 0x01 && hda.converters[0].channel_count == 1 && hda.converters[0].stream == 0x10 && hda.converters[0].format == 0x0011);
  // the other pins and converters are left alone
  CHECK(hda.pins[1].control == 0 && hda.pins[2].control == 0 && hda.converters[1].stream == 0 && hda.converters[2].digital == 0 && hda.pins[1].dip.empty());
  // the audio InfoFrame: type 0x84, version 1, ten data bytes, the checksum making the sum zero, two channels (the count minus one, 1, in the first data byte), the rest zero
  CHECK(hda.pins[0].dip.size() == 14 && hda.pins[0].dip[0] == 0x84 && hda.pins[0].dip[1] == 0x01 && hda.pins[0].dip[2] == 0x0A && hda.pins[0].dip[4] == 0x01);
  uint8_t sum = 0;
  for (uint8_t b : hda.pins[0].dip) sum = static_cast<uint8_t>(sum + b);
  CHECK(sum == 0 && hda.pins[0].dip[3] == static_cast<uint8_t>(0 - (0x84 + 0x01 + 0x0A + 0x01)));
  for (size_t i = 5; i < 14; ++i) CHECK(hda.pins[0].dip[i] == 0);
  CHECK(hda.pins[0].dip_index == 0 && hda.pins[0].dip_xmit == 0xC0);      // packet 0 from byte 0, sent every frame
  // in the order the specification asks for: stop transmitting, set the index, the fourteen bytes, start; and before the stream is assigned
  size_t stop = 0, index = 0, first_data = 0, last_data = 0, start = 0, stream = 0;
  for (size_t i = 0; i < hda.log.size(); ++i) {
    const FakeHda::Verb& v = hda.log[i];
    if (v.nid == 3 && v.command == 0x73200) stop = i;
    if (v.nid == 3 && v.command == 0x73000) index = i;
    if (v.nid == 3 && (v.command >> 8) == 0x731) { if (!first_data) first_data = i; last_data = i; }
    if (v.nid == 3 && v.command == 0x732C0) start = i;
    if (v.nid == 2 && (v.command >> 8) == 0x706) stream = i;
  }
  CHECK(stop && index > stop && first_data > index && last_data == first_data + 13 && start > last_data && stream > start);
  CHECK(count_verbs(hda, 3, 0x70740) == 1 && count_verbs(hda, 2, 0x70D01) == 1 && count_verbs(hda, 2, 0x72D01) == 1);
  dev.shutdown();
}

static void test_pin_choice() {
  struct Case {
    uint32_t sense[3];
    bool none[3];
    uint32_t pin, converter;
    bool display;
  };
  const Case cases[] = {
      {{0, 0, 0}, {false, false, false}, 3, 2, false},                      // no display: the first pin
      {{0, kPd, 0}, {false, false, false}, 5, 2, false},                    // a display without a valid ELD (yet)
      {{0, kPd, kPd | kEldv}, {false, false, false}, 7, 2, true},           // the one with the ELD wins over one without
      {{kPd | kEldv, kPd, kPd | kEldv}, {false, false, false}, 3, 2, true},     // the first of equals
      {{0, 0, kEldv}, {false, false, false}, 3, 2, false},                  // a valid ELD without presence detect is no display
      {{kPd, kPd | kEldv, kPd}, {false, false, false}, 5, 2, true},
      {{0, 0, 0}, {true, false, false}, 5, 2, false},                       // a pin with no physical connection is not a candidate
      {{kPd | kEldv, 0, kPd | kEldv}, {true, false, false}, 7, 2, true},
      {{kPd | kEldv, kPd | kEldv, kPd | kEldv}, {true, true, false}, 7, 2, true},
  };
  for (const Case& c : cases) {
    TestAllocator alloc;
    FakeHda hda;
    for (int i = 0; i < 3; ++i) {
      hda.pins[i].sense = c.sense[i];
      hda.pin_config_none[i] = c.none[i];
    }
    IntelHdaAudioDevice dev(hda, alloc);
    CHECK(dev.initialize() && dev.output_pin() == c.pin && dev.output_converter() == c.converter && dev.output_has_display() == c.display && dev.output_is_digital());
    dev.shutdown();
  }
  // only the pin that is used is configured
  TestAllocator alloc;
  FakeHda hda;
  hda.pins[2].sense = kPd | kEldv;
  IntelHdaAudioDevice dev(hda, alloc);
  CHECK(dev.initialize() && dev.output_pin() == 7);
  CHECK(hda.pins[2].control == 0x40 && hda.pins[0].control == 0 && hda.pins[1].control == 0 && hda.pins[2].dip.size() == 14 && hda.pins[0].dip.empty() && hda.pins[2].select == 0);
  dev.shutdown();
  // every pin without a connection: no output
  FakeHda none;
  none.pin_config_none[0] = none.pin_config_none[1] = none.pin_config_none[2] = true;
  IntelHdaAudioDevice dev2(none, alloc);
  CHECK(!dev2.initialize());
}

static void test_analog_preferred() {
  TestAllocator alloc;
  FakeHda hda;
  hda.analog_pin = true;
  hda.pins[2].sense = kPd | kEldv;      // a display is there, but the line out comes first
  IntelHdaAudioDevice dev(hda, alloc);
  CHECK(dev.initialize() && dev.output_pin() == 8 && dev.output_converter() == 9 && !dev.output_is_digital() && !dev.output_has_display());
  CHECK(hda.line_out.control == 0x40 && hda.line_dac.stream == 0x10 && hda.line_dac.format == 0x0011);
  // nothing digital is set up for an analog pin
  CHECK(hda.line_dac.digital == 0 && hda.line_dac.channel_count == 0 && hda.line_out.dip.empty() && hda.pins[2].control == 0 && hda.converters[0].stream == 0);
  dev.shutdown();
}

static void test_stream_and_playback() {
  TestAllocator alloc;
  FakeHda hda;
  hda.pins[0].sense = kPd | kEldv;
  IntelHdaAudioDevice dev(hda, alloc);
  CHECK(dev.initialize());
  // the controller: out of reset, the rings on, the stream running through its list
  CHECK((hda.load(0x08, 4) & 1) == 1 && (hda.mem[0x4C] & 2) && (hda.mem[0x5C] & 2) && hda.corb_entries() == 256 && hda.rirb_entries() == 256);
  CHECK(hda.stream_resets == 1 && (hda.mem[0x80] & 2) && hda.mem[0x82] == 0x10 && hda.load(0x88, 4) == 8 * 8192 && hda.load(0x8C, 2) == 7 && hda.load(0x92, 2) == 0x0011);
  const uint64_t list = hda.load(0x98, 4) | (uint64_t{hda.load(0x9C, 4)} << 32);
  CHECK(list != 0 && (list % 128) == 0);
  uint64_t first = 0;
  for (uint32_t i = 0; i < 8; ++i) {
    struct { uint64_t address; uint32_t length, flags; } entry;
    memcpy(&entry, reinterpret_cast<const void*>(list + i * 16), 16);
    if (i == 0) first = entry.address;
    CHECK(entry.address == first + uint64_t{i} * 8192 && entry.length == 8192 && entry.flags == 0);
  }
  CHECK(first != 0 && (first % 128) == 0);
  // playing: the samples go to the ring, and the stream's position (LPIB) is what has been played
  std::vector<uint8_t> pcm(4096);
  for (size_t i = 0; i < pcm.size(); ++i) pcm[i] = static_cast<uint8_t>(i * 7);
  CHECK(dev.write(ConstByteView(pcm.data(), pcm.size())) == 4096 && dev.queued_bytes() == 4096);
  CHECK(memcmp(reinterpret_cast<const void*>(first), pcm.data(), 4096) == 0);
  hda.store(0x84, 4, 2048);
  CHECK(dev.queued_bytes() == 2048);
  hda.store(0x84, 4, 4096);
  CHECK(dev.queued_bytes() == 0 && dev.statistics().frames_played == 1024);
  hda.store(0x84, 4, 0);        // the buffer wraps (65536 bytes): played to the end and round again
  CHECK(dev.write(ConstByteView(pcm.data(), 1024)) == 1024);
  // the digital output keeps its state while playing
  CHECK(hda.converters[0].digital == 1 && hda.pins[0].dip_xmit == 0xC0);
  // shutting down stops the stream and the rings and puts the controller in reset
  dev.shutdown();
  CHECK((hda.mem[0x80] & 2) == 0 && hda.mem[0x4C] == 0 && hda.mem[0x5C] == 0 && (hda.load(0x08, 4) & 1) == 0);
  CHECK(dev.write(ConstByteView(pcm.data(), 1024)) == 0);
}

static void test_retarget() {
  TestAllocator alloc;
  FakeHda hda;
  IntelHdaAudioDevice dev(hda, alloc);
  CHECK(!dev.retarget());          // not running
  IAudioDevice& as_device = dev;
  CHECK(!as_device.retarget());    // (and through the interface)
  CHECK(dev.initialize() && dev.output_pin() == 3 && !dev.output_has_display());
  // nothing changed: the same pin, set up again
  CHECK(as_device.retarget() && dev.output_pin() == 3 && hda.pins[0].control == 0x40 && hda.converters[0].stream == 0x10);
  // a display appears on the DisplayPort pin (node 7)
  hda.pins[2].sense = kPd | kEldv;
  CHECK(dev.retarget() && dev.output_pin() == 7 && dev.output_has_display());
  // the old pin is silenced: no output, no InfoFrame; the new one is configured
  CHECK(hda.pins[0].control == 0 && hda.pins[0].dip_xmit == 0x00);
  CHECK(hda.pins[2].control == 0x40 && hda.pins[2].dip_xmit == 0xC0 && hda.pins[2].dip.size() == 14 && hda.pins[2].select == 0);
  CHECK(hda.converters[0].digital == 1 && hda.converters[0].stream == 0x10 && hda.converters[0].channel_count == 1 && hda.converters[0].format == 0x0011);
  // each retarget silences the path first (two so far: the converter's digital output and pin 3's output were switched off twice), and sets the new pin up once
  CHECK(count_verbs(hda, 2, 0x70D00) == 2 && count_verbs(hda, 3, 0x70700) == 2 && count_verbs(hda, 7, 0x70740) == 1 && count_verbs(hda, 3, 0x70740) == 2);
  // playback goes on through the retarget
  std::vector<uint8_t> pcm(512, 0x55);
  CHECK(dev.write(ConstByteView(pcm.data(), pcm.size())) == 512);
  // the display moves to the first pin
  hda.pins[2].sense = 0;
  hda.pins[0].sense = kPd | kEldv;
  CHECK(dev.retarget() && dev.output_pin() == 3 && hda.pins[2].control == 0 && hda.pins[2].dip_xmit == 0 && hda.pins[0].control == 0x40);
  // and a pin that is gone altogether leaves nothing to play to: false, and silent
  hda.pin_config_none[0] = hda.pin_config_none[1] = hda.pin_config_none[2] = true;
  CHECK(!dev.retarget() && hda.pins[0].control == 0 && hda.converters[0].stream == 0);
  dev.shutdown();
  CHECK(!dev.retarget());
}

static void test_failures() {
  TestAllocator alloc;
  {
    FakeHda hda;
    hda.no_codec = true;
    IntelHdaAudioDevice dev(hda, alloc);
    CHECK(!dev.initialize() && hda.load(0x08, 4) == 0);        // the controller is put back in reset
  }
  {
    FakeHda hda;
    hda.no_output_streams = true;
    IntelHdaAudioDevice dev(hda, alloc);
    CHECK(!dev.initialize() && hda.responses == 0);
  }
  {
    FakeHda hda;
    hda.no_converters = true;
    IntelHdaAudioDevice dev(hda, alloc);
    CHECK(!dev.initialize());
  }
  {
    FakeHda hda;
    hda.codec_ignores_rates = true;
    IntelHdaAudioDevice dev(hda, alloc);
    CHECK(!dev.initialize());
  }
  {
    FakeHda hda;
    IntelHdaAudioDevice dev(hda, alloc);
    CHECK(dev.initialize());
    dev.shutdown();
    CHECK(dev.initialize());        // again, from the start
    CHECK(dev.output_pin() == 3 && hda.stream_resets == 2);
    dev.shutdown();
  }
}

int main() {
  test_digital_pin_without_display();
  test_pin_choice();
  test_analog_preferred();
  test_stream_and_playback();
  test_retarget();
  test_failures();
  printf(g_failed ? "IntelHdaTest: %d FAILED\n" : "IntelHdaTest: all passed\n", g_failed);
  return g_failed ? 1 : 0;
}
