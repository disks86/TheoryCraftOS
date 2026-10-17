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

// Host-side tests for the audio ring bookkeeping (PlaybackRing) and the software
// audio device that is built on it.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "Hardware/AudioDevice/PlaybackRing.hpp"
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

static int g_failed = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAILED line %d: %s\n", __LINE__, #cond); ++g_failed; } } while (0)

static void fill(uint8_t* data, uint32_t bytes, uint8_t start) { for (uint32_t i = 0; i < bytes; ++i) data[i] = static_cast<uint8_t>(start + i); }

static void test_ring_basics() {
  TestAllocator a;
  PlaybackRing ring;
  CHECK(!ring.allocate(a, 0));
  CHECK(!ring.allocate(a, 10));  // not a whole number of frames
  CHECK(ring.allocate(a, 64));
  CHECK(ring.free() == 64 && ring.queued() == 0);

  uint8_t data[40];
  fill(data, sizeof data, 1);
  CHECK(ring.write(ConstByteView(data, 38)) == 36);  // whole frames only
  CHECK(ring.queued() == 36 && ring.free() == 28);
  CHECK(memcmp(ring.data(), data, 36) == 0);
  CHECK(ring.write(ConstByteView(data, 40)) == 28);  // only what fits
  CHECK(ring.free() == 0 && ring.write(ConstByteView(data, 4)) == 0);

  ring.advance(16);  // the device has read 16 bytes
  CHECK(ring.queued() == 48 && ring.frames_played() == 4);
  for (int i = 0; i < 16; ++i) CHECK(ring.data()[i] == 0);  // played bytes are silence again
  CHECK(ring.write(ConstByteView(data, 16)) == 16);  // wraps round the end
  CHECK(memcmp(ring.data(), data, 16) == 0);

  ring.advance(16);  // no movement
  CHECK(ring.queued() == 64 && ring.underruns() == 0);
  ring.advance(0);  // the device went round once: all 64 bytes consumed, ring empty
  CHECK(ring.queued() == 16 && ring.underruns() == 0);  // the 16 bytes written after the wrap are still queued
  ring.release(a);
  CHECK(a.live == 0);
}

static void test_ring_underrun() {
  TestAllocator a;
  PlaybackRing ring;
  CHECK(ring.allocate(a, 64));
  uint8_t data[16];
  fill(data, sizeof data, 9);
  CHECK(ring.write(ConstByteView(data, 16)) == 16);
  ring.advance(32);  // played 32 bytes but only 16 were queued
  CHECK(ring.queued() == 0 && ring.underruns() == 1 && ring.frames_played() == 4);
  ring.advance(48);  // idle silence is not an underrun
  CHECK(ring.underruns() == 1);
  // Writing continues at the device's position, and the old data does not come back.
  CHECK(ring.write(ConstByteView(data, 16)) == 16);
  CHECK(memcmp(ring.data() + 48, data, 16) == 0);
  CHECK(ring.data()[0] == 0 && ring.data()[16] == 0);
  // advance_by over several trips round the ring consumes everything.
  CHECK(ring.write(ConstByteView(data, 16)) == 16);
  ring.advance_by(64 * 5 + 8);
  CHECK(ring.queued() == 0 && ring.underruns() == 2);
  ring.release(a);
  CHECK(a.live == 0);
}

static void test_software_device() {
  TestAllocator a;
  FakeClock clock;
  SoftwareAudioDevice device(clock, a);
  CHECK(device.write(ConstByteView()) == 0);  // not running yet
  CHECK(device.initialize());
  CHECK(device.free_bytes() == SoftwareAudioDevice::kRingBytes);

  static uint8_t second[kAudioSampleRate * kAudioFrameSize / 10];  // 100 ms
  const uint32_t bytes = sizeof second;
  CHECK(device.write(ConstByteView(second, bytes)) == bytes);
  CHECK(device.queued_bytes() == bytes);

  clock.now += 50000;  // 50 ms later, half is gone
  CHECK(device.queued_bytes() == bytes / 2);
  clock.now += 49999;  // one microsecond short of the end
  CHECK(device.queued_bytes() == kAudioFrameSize);  // one frame left
  clock.now += 100000;
  CHECK(device.queued_bytes() == 0);
  AudioStatistics stats = device.statistics();
  CHECK(stats.frames_queued == bytes / kAudioFrameSize && stats.frames_played == bytes / kAudioFrameSize);
  CHECK(stats.underruns == 1);  // it ran dry at the end of the stream

  // Volume and mute are remembered and clamped.
  device.set_volume(250);
  CHECK(device.volume() == kAudioMaxVolume);
  device.set_muted(true);
  CHECK(device.muted() && device.volume() == kAudioMaxVolume);

  // A full ring refuses more, and accepts again once time has passed.
  static uint8_t big[SoftwareAudioDevice::kRingBytes + 100];
  CHECK(device.write(ConstByteView(big, sizeof big)) == SoftwareAudioDevice::kRingBytes);
  CHECK(device.free_bytes() == 0 && device.write(ConstByteView(big, 4)) == 0);
  clock.now += 1000000;  // a second is much longer than the ring
  CHECK(device.queued_bytes() == 0 && device.free_bytes() == SoftwareAudioDevice::kRingBytes);

  device.shutdown();
  CHECK(device.write(ConstByteView(big, 4)) == 0);
  CHECK(a.live == 0);
}

static void test_out_of_memory() {
  TestAllocator a;
  FakeClock clock;
  a.fail = true;
  SoftwareAudioDevice device(clock, a);
  CHECK(!device.initialize());
  CHECK(a.live == 0);
}

int main() {
  test_ring_basics();
  test_ring_underrun();
  test_software_device();
  test_out_of_memory();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("audio tests passed\n");
  return 0;
}
