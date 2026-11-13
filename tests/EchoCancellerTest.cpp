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

// The echo canceller (src/PipeWire/EchoCanceller.hpp) on synthetic rooms: speech-like far ends (shaped noise in syllables) played through an echo
// path (a delay and a few reflections) into a microphone, with and without a near-end talker.
#include <stdio.h>
#include <stdlib.h>
#include <cmath>
#include <limits>
#include <vector>
#include "PipeWire/EchoCanceller.hpp"

using namespace pipewire;

struct TestAllocator final : allocators::IAllocator {
  void* allocate(uint64_t size) override { return malloc(size ? size : 1); }
  void* allocate_aligned(uint64_t size, uint64_t align) override {
    void* p = nullptr;
    if (posix_memalign(&p, align < sizeof(void*) ? sizeof(void*) : align, size ? size : 1) != 0) return nullptr;
    return p;
  }
  void deallocate(void* p) override { free(p); }
};
static TestAllocator A;
void bounds_violation(const char* what, uint64_t index, uint64_t size) {
  printf("bounds violation: %s %llu of %llu\n", what, (unsigned long long)index, (unsigned long long)size);
  abort();
}

static int g_failed = 0, g_checks = 0;
#define CHECK(...) do { ++g_checks; if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

constexpr uint32_t kBlock = EchoCanceller::kBlock;
constexpr uint32_t kRate = 48000;

// A speech-like signal: noise through a resonance, in "syllables" of 60 .. 250 ms at random loudness with short pauses.
struct Talker {
  explicit Talker(uint32_t seed, float level) : state(seed), level(level) {}
  uint32_t state;
  float level;
  float y1 = 0.0f, y2 = 0.0f, gain = 0.0f, target = 0.0f;
  uint32_t left = 0;
  float uniform() {
    state = state * 1664525u + 1013904223u;
    return static_cast<float>(state >> 8) / 16777216.0f;
  }
  float next() {
    if (left == 0) {
      left = static_cast<uint32_t>((0.06f + 0.19f * uniform()) * kRate);
      target = uniform() < 0.2f ? 0.0f : (0.2f + 0.8f * uniform());
    }
    --left;
    gain += (target - gain) * 0.002f;
    const float noise = uniform() * 2.0f - 1.0f;
    const float y = noise + 1.6f * y1 - 0.8f * y2;      // (a resonance near 3 kHz)
    y2 = y1;
    y1 = y;
    return level * gain * y * 0.25f;
  }
};

// An echo path: a bulk delay then a few reflections.
struct Room {
  struct Tap {
    uint32_t at;
    float gain;
  };
  std::vector<Tap> taps;
  std::vector<float> history = std::vector<float>(1 << 16, 0.0f);
  uint64_t written = 0;
  float play(float far) {
    history[written % history.size()] = far;
    ++written;
    float echo = 0.0f;
    for (const Tap& t : taps)
      if (t.at < written) echo += t.gain * history[(written - 1 - t.at) % history.size()];
    return echo;
  }
};

static Room room(uint32_t delay, float scale) {
  Room r;
  r.taps = {{delay, 0.5f * scale}, {delay + 37, -0.25f * scale}, {delay + 120, 0.15f * scale}, {delay + 700, 0.08f * scale}, {delay + 2100, -0.04f * scale}};
  return r;
}

struct Run {
  double near = 0.0, out = 0.0, talk = 0.0, residual_talk = 0.0;
  void clear() { near = out = talk = residual_talk = 0.0; }
  double erle_db() const { return 10.0 * std::log10(near / (out > 1e-20 ? out : 1e-20)); }
};

// Runs `seconds` through the canceller. With `talker`, its voice is added to the microphone (double talk); `run` sums energies from `measure_from` on.
static void play(EchoCanceller& aec, Talker& far_talker, Room& r, Talker* near_talker, double seconds, double measure_from, Run* run) {
  const uint64_t blocks = static_cast<uint64_t>(seconds * kRate / kBlock), first = static_cast<uint64_t>(measure_from * kRate / kBlock);
  float far[kBlock], near[kBlock], out[kBlock], voice[kBlock];
  for (uint64_t b = 0; b < blocks; ++b) {
    for (uint32_t i = 0; i < kBlock; ++i) {
      far[i] = far_talker.next();
      voice[i] = near_talker ? near_talker->next() : 0.0f;
      near[i] = r.play(far[i]) + voice[i];
    }
    CHECK(aec.process(ArrayView<const float>(far, kBlock), ArrayView<const float>(near, kBlock), ArrayView<float>(out, kBlock)));
    if (b < first || !run) continue;
    for (uint32_t i = 0; i < kBlock; ++i) {
      const double echo = near[i] - voice[i];
      run->near += echo * echo;
      run->out += (out[i] - voice[i]) * (out[i] - voice[i]);      // (what is left of the echo)
      run->talk += voice[i] * voice[i];
    }
  }
}

static void test_short_echo() {
  // an echo the filter covers on its own (42 ms): no bulk delay needed
  EchoCanceller aec(A);
  CHECK(aec.initialize(16, 0));
  Talker far(1, 1.0f);
  Room r = room(2000, 1.0f);
  Run run;
  play(aec, far, r, nullptr, 6.0, 3.0, &run);
  printf("short echo: ERLE %.1f dB (%u foreground updates)\n", run.erle_db(), aec.foreground_updates());
  CHECK(run.erle_db() > 20.0);
  CHECK(aec.foreground_updates() > 0);
  CHECK(aec.erle() > 30.0f);
  CHECK(aec.delay() == 0);
}

static void test_long_echo() {
  // 250 ms of buffering before the room: the delay estimator must find it
  EchoCanceller aec(A);
  CHECK(aec.initialize(16, 24000));
  Talker far(7, 0.8f);
  Room r = room(12000, 0.8f);
  Run run;
  play(aec, far, r, nullptr, 10.0, 7.0, &run);
  printf("long echo: delay %u (true 12000), ERLE %.1f dB\n", aec.delay(), run.erle_db());
  CHECK(aec.delay() <= 12000 && aec.delay() + 1200 >= 12000);
  CHECK(aec.delay_updates() >= 1);
  CHECK(run.erle_db() > 18.0);
}

static void test_double_talk() {
  EchoCanceller aec(A);
  CHECK(aec.initialize(16, 24000));
  Talker far(3, 1.0f), voice(99, 0.7f);
  Room r = room(3000, 0.9f);
  play(aec, far, r, nullptr, 5.0, 5.0, nullptr);      // (converge first)
  Run run;
  play(aec, far, r, &voice, 3.0, 0.0, &run);
  // the near talker comes through (what is left besides the voice is small next to it) and the echo stays cancelled
  printf("double talk: echo left %.1f dB under the echo, voice/residual %.1f dB, %u background resets\n", run.erle_db(),
         10.0 * std::log10(run.talk / (run.out > 1e-20 ? run.out : 1e-20)), aec.background_resets());
  CHECK(run.erle_db() > 12.0);
  CHECK(run.talk > 10.0 * run.out);
  Run after;
  play(aec, far, r, nullptr, 2.0, 0.5, &after);
  printf("after double talk: ERLE %.1f dB\n", after.erle_db());
  CHECK(after.erle_db() > 18.0);
}

static void test_path_change() {
  // the room changes (a laptop moved): the background filter finds the new path and replaces the foreground
  EchoCanceller aec(A);
  CHECK(aec.initialize(16, 0));
  Talker far(5, 1.0f);
  Room r = room(1500, 1.0f);
  play(aec, far, r, nullptr, 4.0, 4.0, nullptr);
  const uint32_t updates = aec.foreground_updates();
  Room moved = room(1800, -0.7f);
  Run run;
  play(aec, far, moved, nullptr, 5.0, 3.0, &run);
  printf("path change: ERLE %.1f dB, %u foreground updates (were %u)\n", run.erle_db(), aec.foreground_updates(), updates);
  CHECK(aec.foreground_updates() > updates);
  CHECK(run.erle_db() > 18.0);
}

static void test_silence_and_passthrough() {
  // nothing played: the microphone passes unchanged
  EchoCanceller aec(A);
  CHECK(aec.initialize(4, 4800));
  Talker voice(11, 0.5f);
  float far[kBlock] = {}, near[kBlock], out[kBlock];
  bool same = true;
  for (int b = 0; b < 200; ++b) {
    for (float& v : near) v = voice.next();
    CHECK(aec.process(ArrayView<const float>(far, kBlock), ArrayView<const float>(near, kBlock), ArrayView<float>(out, kBlock)));
    for (uint32_t i = 0; i < kBlock; ++i) same = same && out[i] == near[i];
  }
  CHECK(same);
  CHECK(aec.erle() == 1.0f);
  CHECK(aec.foreground_updates() == 0);
}

static void test_bad_input() {
  // NaNs, infinities and huge values neither poison the filter nor come out; it still converges afterwards
  EchoCanceller aec(A);
  CHECK(aec.initialize(8, 4800));
  Talker far(21, 1.0f);
  Room r = room(500, 1.0f);
  float f[kBlock], n[kBlock], out[kBlock];
  const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
  bool finite = true;
  for (int b = 0; b < 400; ++b) {
    for (uint32_t i = 0; i < kBlock; ++i) {
      f[i] = far.next();
      n[i] = r.play(f[i]);
    }
    if (b % 37 == 5) {
      f[3] = nan;
      n[100] = inf;
      f[200] = -inf;
      n[7] = 1e30f;
      f[9] = -1e30f;
    }
    CHECK(aec.process(ArrayView<const float>(f, kBlock), ArrayView<const float>(n, kBlock), ArrayView<float>(out, kBlock)));
    for (float v : out) finite = finite && std::isfinite(v) && std::fabs(v) < 100.0f;
  }
  CHECK(finite);
  Run run;
  play(aec, far, r, nullptr, 4.0, 2.0, &run);
  printf("after bad input: ERLE %.1f dB\n", run.erle_db());
  CHECK(run.erle_db() > 15.0);
  // bit flips in the far end and the microphone (random sample words with one bit turned over): the output stays finite
  uint32_t seed = 12345;
  finite = true;
  for (int b = 0; b < 300; ++b) {
    for (uint32_t i = 0; i < kBlock; ++i) {
      f[i] = far.next();
      n[i] = r.play(f[i]);
      seed = seed * 1103515245u + 12345u;
      if ((seed >> 16) % 50 == 0) {
        float* target = (seed & 1) ? &f[i] : &n[i];
        uint32_t bits;
        __builtin_memcpy(&bits, target, 4);
        bits ^= 1u << ((seed >> 8) % 32);
        __builtin_memcpy(target, &bits, 4);
      }
    }
    CHECK(aec.process(ArrayView<const float>(f, kBlock), ArrayView<const float>(n, kBlock), ArrayView<float>(out, kBlock)));
    for (float v : out) finite = finite && std::isfinite(v) && std::fabs(v) < 100.0f;
  }
  CHECK(finite);
}

static void test_sizes() {
  EchoCanceller aec(A);
  float buffer[kBlock * 2] = {};
  CHECK(!aec.process(ArrayView<const float>(buffer, kBlock), ArrayView<const float>(buffer, kBlock), ArrayView<float>(buffer, kBlock)));      // (not initialized)
  CHECK(!aec.initialize(0, 0));
  CHECK(!aec.initialize(EchoCanceller::kMaxPartitions + 1, 0));
  CHECK(!aec.initialize(4, EchoCanceller::kMaxDelayLimit + 1));
  CHECK(aec.initialize(EchoCanceller::kMaxPartitions, EchoCanceller::kMaxDelayLimit));
  CHECK(aec.partitions() == EchoCanceller::kMaxPartitions);
  float out[kBlock];
  // truncated and oversized blocks are refused
  CHECK(!aec.process(ArrayView<const float>(buffer, kBlock - 1), ArrayView<const float>(buffer, kBlock), ArrayView<float>(out, kBlock)));
  CHECK(!aec.process(ArrayView<const float>(buffer, kBlock), ArrayView<const float>(buffer, kBlock + 1), ArrayView<float>(out, kBlock)));
  CHECK(!aec.process(ArrayView<const float>(buffer, kBlock), ArrayView<const float>(buffer, kBlock), ArrayView<float>(out, kBlock - 1)));
  CHECK(!aec.process(ArrayView<const float>(buffer, 0), ArrayView<const float>(buffer, 0), ArrayView<float>(out, 0)));
  CHECK(aec.process(ArrayView<const float>(buffer, kBlock), ArrayView<const float>(buffer, kBlock), ArrayView<float>(out, kBlock)));
}

int main() {
  test_sizes();
  test_silence_and_passthrough();
  test_short_echo();
  test_long_echo();
  test_double_talk();
  test_path_change();
  test_bad_input();
  printf("EchoCancellerTest: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
