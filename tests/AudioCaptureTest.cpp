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

// Host-side tests for audio capture: CaptureRing, the software input, and the
// capture hub with its streams over a fake input device.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "Audio/AudioCaptureHub.hpp"
#include "Hardware/AudioDevice/CaptureRing.hpp"
#include "Hardware/AudioDevice/Software/SoftwareAudioDevice.hpp"

using namespace hardware;

struct TestAllocator final : allocators::IAllocator {
  int live = 0; bool fail = false;
  void* allocate(uint64_t size) override { if (fail) return nullptr; ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

struct FakeClock final : IClock {
  uint64_t now = 0;
  const char* name() const override { return "fake clock"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint64_t now_us() const override { return now; }
};

// A microphone that returns what the test puts in.
struct FakeInput final : IAudioCapture {
  uint8_t pending[8192]; uint32_t length = 0;
  uint8_t level = 100; bool mute = false; int flushes = 0;
  const char* name() const override { return "fake input"; }
  void add(const int16_t* samples, uint32_t frames) { memcpy(pending + length, samples, frames * kAudioFrameSize); length += frames * kAudioFrameSize; }
  uint32_t read(ByteView out) override {
    uint32_t n = out.size() < length ? (uint32_t)out.size() : length;
    n -= n % kAudioFrameSize;
    memcpy(out.data(), pending, n);
    memmove(pending, pending + n, length - n);
    length -= n;
    return n;
  }
  uint32_t available_bytes() override { return length; }
  void flush() override { length = 0; ++flushes; }
  void set_gain(uint8_t g) override { level = g; }
  uint8_t gain() const override { return level; }
  void set_muted(bool m) override { mute = m; }
  bool muted() const override { return mute; }
  AudioCaptureStatistics statistics() override { return {}; }
};

static int g_failed = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAILED line %d: %s\n", __LINE__, #cond); ++g_failed; } } while (0)

static void fill(uint8_t* data, uint32_t bytes, uint8_t start) { for (uint32_t i = 0; i < bytes; ++i) data[i] = static_cast<uint8_t>(start + i); }

static void test_ring() {
  TestAllocator a;
  CaptureRing ring;
  CHECK(!ring.allocate(a, 10) && !ring.allocate(a, 0));
  CHECK(ring.allocate(a, 64));
  uint8_t out[80];

  // The device writes 24 bytes (positions 0..24).
  fill(ring.data(), 24, 1);
  ring.advance(24);
  CHECK(ring.available() == 24 && ring.frames_captured() == 6);
  CHECK(ring.read(ByteView(out, 10)) == 8);  // whole frames only
  uint8_t expect[24]; fill(expect, 24, 1);
  CHECK(memcmp(out, expect, 8) == 0);
  CHECK(ring.read(ByteView(out, 80)) == 16 && memcmp(out, expect + 8, 16) == 0);
  CHECK(ring.available() == 0 && ring.frames_read() == 6);

  // The device wraps round the end: 24 -> 64 -> 8.
  fill(ring.data() + 24, 40, 100);
  fill(ring.data(), 8, 140);
  ring.advance(8);
  CHECK(ring.available() == 48 && ring.overruns() == 0);
  CHECK(ring.read(ByteView(out, 80)) == 48);
  CHECK(out[0] == 100 && out[39] == 139 && out[40] == 140 && out[47] == 147);

  // The program falls behind by more than the buffer: the oldest audio is lost.
  ring.advance(8 + 0);  // no movement
  ring.advance(4);      // 60 bytes later (nearly a whole round): 60 bytes new
  CHECK(ring.available() == 60 && ring.overruns() == 0);
  ring.advance(60);     // another 56: 116 queued > 64
  CHECK(ring.available() == 64 && ring.overruns() == 1 && ring.frames_lost() == (116 - 64) / 4);
  ring.flush();
  CHECK(ring.available() == 0);

  // push() copies blocks in (virtio-style) and wraps.
  ring.reset();
  const uint64_t lost_before = ring.frames_lost(), overruns_before = ring.overruns();
  uint8_t block[48]; fill(block, 48, 7);
  ring.push(ConstByteView(block, 48));
  ring.push(ConstByteView(block, 48));  // 96 > 64: the first 32 bytes are lost
  CHECK(ring.available() == 64 && ring.overruns() == overruns_before + 1 && ring.frames_lost() == lost_before + 8);
  CHECK(ring.read(ByteView(out, 80)) == 64);
  CHECK(memcmp(out, block + 32, 16) == 0);  // the end of the first block,
  CHECK(memcmp(out + 16, block, 48) == 0);  // then all of the second
  ring.reset();
  uint8_t big[200]; fill(big, 200, 0);
  ring.push(ConstByteView(big, 200));  // only the last 64 survive
  CHECK(ring.available() == 64 && ring.read(ByteView(out, 80)) == 64 && memcmp(out, big + 136, 64) == 0);
  ring.release(a);
  CHECK(a.live == 0);
}

static void test_software_input() {
  TestAllocator a; FakeClock clock;
  SoftwareAudioDevice device(clock, a);
  CHECK(device.capture() == nullptr);  // not running yet
  CHECK(device.initialize());
  IAudioCapture* in = device.capture();
  CHECK(in && in->available_bytes() == 0);
  clock.now += 10000;  // 10 ms = 480 frames
  CHECK(in->available_bytes() == 480 * kAudioFrameSize);
  uint8_t out[4096]; memset(out, 0xAA, sizeof out);
  CHECK(in->read(ByteView(out, 1000)) == 1000);
  for (int i = 0; i < 1000; ++i) CHECK(out[i] == 0);  // silence
  in->flush();
  CHECK(in->available_bytes() == 0);
  clock.now += 1000000;  // a second: more than the ring holds
  CHECK(in->available_bytes() == SoftwareAudioCapture::kRingBytes);
  AudioCaptureStatistics stats = in->statistics();
  CHECK(stats.overruns == 1 && stats.frames_lost > 0);
  in->set_gain(40); in->set_muted(true);
  CHECK(in->gain() == 40 && in->muted());
  device.shutdown();
  CHECK(device.capture() == nullptr && a.live == 0);
}

static void test_hub() {
  TestAllocator a; FakeInput input;
  audio::AudioCaptureHub hub(input, a);
  CHECK(hub.initialize());
  int16_t pcm[100 * 2];
  for (int i = 0; i < 200; ++i) pcm[i] = static_cast<int16_t>(1000 + i);

  input.add(pcm, 100);
  hub.pump();  // nobody listening: discarded
  CHECK(input.length == 0);

  auto s1 = hub.open_stream(4096);
  CHECK(s1 && input.flushes >= 1);
  input.add(pcm, 100);
  auto s2 = hub.open_stream(4096);  // a second listener hears the same from now on
  CHECK(s2 && hub.stream_count() == 2);
  hub.pump();
  CHECK(s1->available_bytes() == 400 && s2->available_bytes() == 400);
  uint8_t out[400];
  CHECK(s1->read(ByteView(out, 400)) == 400 && memcmp(out, pcm, 400) == 0);
  CHECK(s2->available_bytes() == 400);  // independent of the first

  // Per-stream gain and mute.
  s2->set_gain(50);
  CHECK(s2->read(ByteView(out, 400)) == 400);
  CHECK(((int16_t*)out)[0] == 500 && ((int16_t*)out)[199] == (1199 * 50 / 100));
  s2->set_muted(true);
  input.add(pcm, 100);
  hub.pump();
  CHECK(s2->read(ByteView(out, 400)) == 400 && ((int16_t*)out)[5] == 0);
  CHECK(s1->available_bytes() == 400);

  // A stream that never reads loses the oldest audio but does not hurt the others.
  s1->flush();
  for (int i = 0; i < 12; ++i) { input.add(pcm, 100); hub.pump(); s2->flush(); }
  CHECK(s1->statistics().overruns >= 1 && s1->available_bytes() == 4096);
  CHECK(s2->statistics().overruns == 0);

  s1->close();
  hub.pump();
  CHECK(hub.stream_count() == 1);
  s2->close();
  hub.pump();
  CHECK(hub.stream_count() == 0);

  hub.set_master_gain(33); hub.set_master_muted(true);
  CHECK(input.level == 33 && input.mute && hub.master_gain() == 33 && hub.master_muted());
  CHECK(hub.statistics().frames_distributed >= 300 && hub.statistics().streams_opened == 2);

  SharedPointer<audio::CaptureStream> many[audio::AudioCaptureHub::kMaxStreams];
  for (auto& p : many) { p = hub.open_stream(64); CHECK(p); }
  CHECK(!hub.open_stream(64));
  for (auto& p : many) p.reset();
}

static void test_capture_formats() {
  TestAllocator a; FakeInput input;
  audio::AudioCaptureHub hub(input, a);
  CHECK(hub.initialize());
  CHECK(!hub.open_stream(audio::StreamFormat{300000, 2, audio::SampleType::S16}));

  // 16 kHz mono 8-bit: a third of the frames, one byte each, the two channels averaged.
  auto voice = hub.open_stream(audio::StreamFormat{16000, 1, audio::SampleType::U8}, 4096);
  auto raw = hub.open_stream(4096);
  CHECK(voice && raw);
  int16_t pcm[300 * 2];
  for (int i = 0; i < 300; ++i) { pcm[2 * i] = 8192; pcm[2 * i + 1] = 8192; }  // 0.25 of full scale
  input.add(pcm, 300);
  hub.pump();
  CHECK(raw->available_bytes() == 1200);
  const uint32_t got = voice->available_bytes();
  CHECK(got >= 98 && got <= 100);
  uint8_t out[200];
  CHECK(voice->read(ByteView(out, 200)) == got);
  for (uint32_t i = 1; i < got; ++i) CHECK(out[i] == 128 + 32);  // 8192 / 256 = 32

  // Gain on an 8-bit stream scales around the middle.
  voice->set_gain(50);
  input.add(pcm, 300);
  hub.pump();
  CHECK(voice->read(ByteView(out, 200)) > 90 && out[10] == 128 + 16);
  voice->set_muted(true);
  input.add(pcm, 300);
  hub.pump();
  CHECK(voice->read(ByteView(out, 200)) > 90 && out[10] == 128);

  // Upsampling to 96 kHz doubles the frames, and a stream with a pending conversion keeps up across pumps.
  auto wide = hub.open_stream(audio::StreamFormat{96000, 2, audio::SampleType::S16}, 8192);
  input.add(pcm, 100);
  hub.pump();
  input.add(pcm, 100);
  hub.pump();
  CHECK(wide->available_bytes() >= 4 * 398 && wide->available_bytes() <= 4 * 400);
}

int main() {
  test_ring();
  test_software_input();
  test_hub();
  test_capture_formats();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("audio capture tests passed\n");
  return 0;
}
