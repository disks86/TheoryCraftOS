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

// Host-side tests for the sound server: AudioStream and AudioMixer over a fake
// audio device that records what it is given.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "Audio/AudioMixer.hpp"

using namespace hardware;

struct TestAllocator final : allocators::IAllocator {
  int live = 0; bool fail = false;
  void* allocate(uint64_t size) override { if (fail) return nullptr; ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

// Records everything written; "plays" instantly when told to.
struct FakeDevice final : IAudioDevice {
  static constexpr uint32_t kCapacity = 16384;
  int16_t samples[kCapacity / 2] = {};
  uint32_t recorded = 0;     // bytes recorded in total
  uint32_t queued = 0;       // bytes accepted and not yet "played"
  uint32_t accept_limit = ~0u;  // at most this many bytes per write
  uint8_t vol = 80; bool mute = false;
  const char* name() const override { return "fake audio"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint32_t write(ConstByteView pcm) override {
    uint32_t n = pcm.size() < kCapacity - queued ? (uint32_t)pcm.size() : kCapacity - queued;
    if (n > accept_limit) n = accept_limit;
    n -= n % kAudioFrameSize;
    if (recorded + n > sizeof samples) n = (uint32_t)sizeof samples - recorded;
    memcpy((uint8_t*)samples + recorded, pcm.data(), n);
    recorded += n; queued += n;
    return n;
  }
  uint32_t queued_bytes() override { return queued; }
  uint32_t free_bytes() override { return kCapacity - queued; }
  void play_all() { queued = 0; }
  void set_volume(uint8_t v) override { vol = v; }
  uint8_t volume() const override { return vol; }
  void set_muted(bool m) override { mute = m; }
  bool muted() const override { return mute; }
  AudioStatistics statistics() override { return {}; }
};

static int g_failed = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAILED line %d: %s\n", __LINE__, #cond); ++g_failed; } } while (0)

// n stereo frames, every sample = value.
static uint32_t constant_frames(int16_t* out, uint32_t n, int16_t value) {
  for (uint32_t i = 0; i < n * 2; ++i) out[i] = value;
  return n * kAudioFrameSize;
}

static void test_single_stream() {
  TestAllocator a; FakeDevice dev;
  audio::AudioMixer mixer(dev, a);
  CHECK(mixer.initialize());
  auto s = mixer.open_stream(4096);
  CHECK(s && mixer.stream_count() == 1);
  int16_t pcm[100 * 2];
  CHECK(s->write(ConstByteView((uint8_t*)pcm, constant_frames(pcm, 100, 1000))) == 400);
  CHECK(s->queued_bytes() == 400);
  mixer.pump();
  CHECK(dev.recorded == 400 && s->queued_bytes() == 0);
  for (uint32_t i = 0; i < 200; ++i) CHECK(dev.samples[i] == 1000);
  mixer.pump();  // nothing more queued: nothing is written
  CHECK(dev.recorded == 400);
  CHECK(mixer.statistics().frames_mixed == 100);
}

static void test_mixing_and_clipping() {
  TestAllocator a; FakeDevice dev;
  audio::AudioMixer mixer(dev, a);
  CHECK(mixer.initialize());
  auto x = mixer.open_stream(4096), y = mixer.open_stream(4096), z = mixer.open_stream(4096);
  int16_t pcm[50 * 2];
  uint32_t bytes = constant_frames(pcm, 50, 1000);
  x->write(ConstByteView((uint8_t*)pcm, bytes));
  bytes = constant_frames(pcm, 30, 2000);
  y->write(ConstByteView((uint8_t*)pcm, bytes));
  y->set_volume(50);        // 2000 -> 1000
  bytes = constant_frames(pcm, 10, 3000);
  z->write(ConstByteView((uint8_t*)pcm, bytes));
  z->set_muted(true);       // contributes nothing but still drains
  mixer.pump();
  CHECK(dev.recorded == 50 * 4);                       // as long as the longest stream
  CHECK(dev.samples[0] == 1000 + 1000);                // x + y at half volume
  CHECK(dev.samples[2 * 29 + 1] == 2000);              // last frame where y still plays
  CHECK(dev.samples[2 * 30] == 1000 && dev.samples[2 * 49] == 1000);  // x alone
  CHECK(z->queued_bytes() == 0);

  // Clipping at both ends.
  bytes = constant_frames(pcm, 4, 30000);
  x->write(ConstByteView((uint8_t*)pcm, bytes));
  y->set_volume(100);
  y->write(ConstByteView((uint8_t*)pcm, bytes));
  uint32_t before = dev.recorded / 2;
  mixer.pump();
  CHECK(dev.samples[before] == 32767);
  bytes = constant_frames(pcm, 4, -30000);
  x->write(ConstByteView((uint8_t*)pcm, bytes));
  y->write(ConstByteView((uint8_t*)pcm, bytes));
  before = dev.recorded / 2;
  mixer.pump();
  CHECK(dev.samples[before] == -32768);
  CHECK(mixer.statistics().clipped_samples == 16);
}

static void test_close_and_limits() {
  TestAllocator a; FakeDevice dev;
  audio::AudioMixer mixer(dev, a);
  CHECK(mixer.initialize());
  auto s = mixer.open_stream(4096);
  int16_t pcm[10 * 2];
  uint32_t bytes = constant_frames(pcm, 10, 5);
  s->write(ConstByteView((uint8_t*)pcm, bytes));
  s->close();
  CHECK(s->write(ConstByteView((uint8_t*)pcm, bytes)) == 0);  // closed: no more
  CHECK(mixer.stream_count() == 1);
  mixer.pump();  // plays what was queued, then lets go on the next pump
  CHECK(dev.recorded == 40);
  mixer.pump();
  CHECK(mixer.stream_count() == 0);

  // The stream limit and the ring size.
  SharedPointer<audio::AudioStream> all[audio::AudioMixer::kMaxStreams];
  for (auto& p : all) { p = mixer.open_stream(64); CHECK(p); }
  CHECK(!mixer.open_stream(64));
  uint8_t big[200] = {};
  CHECK(all[0]->write(ConstByteView(big, 200)) == 64);  // ring holds 64 bytes
  CHECK(all[0]->write(ConstByteView(big, 6)) == 0 && all[0]->free_bytes() == 0);
  for (auto& p : all) p.reset();
  mixer.pump();  // dropped by clients but not closed: they stay
  CHECK(mixer.stream_count() == audio::AudioMixer::kMaxStreams);

  TestAllocator b; b.fail = true;
  audio::AudioMixer broken(dev, b);
  CHECK(!broken.initialize());
}

static void test_device_pressure() {
  TestAllocator a; FakeDevice dev;
  audio::AudioMixer mixer(dev, a);
  CHECK(mixer.initialize());
  auto s = mixer.open_stream(16384);
  static int16_t pcm[2000 * 2];
  uint32_t bytes = constant_frames(pcm, 2000, 7);
  CHECK(s->write(ConstByteView((uint8_t*)pcm, bytes)) == bytes);

  // A device that takes only a little at a time: nothing is lost or repeated.
  dev.accept_limit = 100;
  for (int i = 0; i < 100 && dev.recorded < bytes; ++i) mixer.pump();
  CHECK(dev.recorded == bytes);
  for (uint32_t i = 0; i < 4000; ++i) CHECK(dev.samples[i] == 7);

  // The mixer keeps the device queue to about 50 ms (2400 frames).
  dev.accept_limit = ~0u; dev.play_all(); dev.recorded = 0;
  bytes = constant_frames(pcm, 2000, 9);
  auto t = mixer.open_stream(16384);
  t->write(ConstByteView((uint8_t*)pcm, bytes));
  t->write(ConstByteView((uint8_t*)pcm, bytes));
  for (int i = 0; i < 10; ++i) mixer.pump();
  CHECK(dev.queued_bytes() / kAudioFrameSize < audio::AudioMixer::kTargetDelayFrames + audio::AudioMixer::kChunkFrames);
  CHECK(dev.queued_bytes() / kAudioFrameSize >= audio::AudioMixer::kTargetDelayFrames);

  mixer.set_master_volume(33); mixer.set_master_muted(true);
  CHECK(dev.vol == 33 && dev.mute && mixer.master_volume() == 33 && mixer.master_muted());
}

int main() {
  test_single_stream();
  test_mixing_and_clipping();
  test_close_and_limits();
  test_device_pressure();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("audio mixer tests passed\n");
  return 0;
}
