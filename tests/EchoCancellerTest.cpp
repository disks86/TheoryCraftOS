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


// A white noise source (a fan, a room's hum) at `level` (its RMS is about level / sqrt(3)).
struct Noise {
  explicit Noise(uint32_t seed, float level) : state(seed), level(level) {}
  uint32_t state;
  float level;
  float next() {
    state = state * 1103515245u + 12345u;
    return level * (static_cast<float>(state >> 8) / 8388608.0f - 1.0f);
  }
};

// A talker that speaks for 400 ms and is quiet for 400 ms, by turns (sample `at` of the run): `speaking` says which.
static bool speaking(uint64_t at) { return (at / 19200) % 2 == 0; }

static void test_high_pass() {
  // a DC offset and 30 Hz rumble on the microphone: gone; a 1 kHz tone kept
  EchoCanceller aec(A);
  CHECK(aec.initialize(4, 0));
  EchoCanceller::Options o;
  o.high_pass_filter = true;
  aec.set_options(o);
  CHECK(aec.latency() == 0);
  float far[kBlock] = {}, near[kBlock], out[kBlock];
  double mean = 0, tone_in = 0, tone_out = 0;
  uint64_t n = 0, at = 0;
  for (int b = 0; b < 400; ++b) {
    for (uint32_t i = 0; i < kBlock; ++i, ++at) {
      const double t = double(at) / kRate;
      near[i] = float(0.3 + 0.2 * std::sin(2 * M_PI * 30 * t) + 0.1 * std::sin(2 * M_PI * 1000 * t));
    }
    CHECK(aec.process(ArrayView<const float>(far, kBlock), ArrayView<const float>(near, kBlock), ArrayView<float>(out, kBlock)));
    if (b < 100) continue;
    for (uint32_t i = 0; i < kBlock; ++i) {
      mean += out[i];
      tone_out += double(out[i]) * out[i];
      const double t = double(at - kBlock + i) / kRate;
      tone_in += 0.01 * std::sin(2 * M_PI * 1000 * t) * std::sin(2 * M_PI * 1000 * t);
      ++n;
    }
  }
  mean /= double(n);
  printf("high pass: mean %.5f, 1 kHz kept %.2f of its energy (with the rumble's rest)\n", mean, tone_out / tone_in);
  CHECK(std::fabs(mean) < 0.002);
  CHECK(tone_out / tone_in > 0.9 && tone_out / tone_in < 1.3);      // (30 Hz is down 17 dB: a little of it is left)
}

// Echo through a loudspeaker that distorts (soft clipping): the linear filter cannot take it all; the suppressor takes most of what is left.
static double nonlinear_run(bool suppress, double* talk_ratio) {
  EchoCanceller aec(A);
  CHECK(aec.initialize(16, 0));
  EchoCanceller::Options o;
  o.echo_suppression = suppress;
  aec.set_options(o);
  Talker far(41, 1.6f), voice(43, 0.5f);
  Room r = room(1500, 1.0f);
  float f[kBlock], n[kBlock], out[kBlock], echo_part[kBlock];
  double echo_in = 0, echo_left = 0, talk_in = 0, talk_out = 0;
  std::vector<float> echoes, voices;      // (what went in, to measure what comes out a latency later)
  const uint32_t latency = aec.latency();
  for (int b = 0; b < 2400; ++b) {
    const bool talk = b >= 1800;      // (the last 1.3 s: a near-end talker too)
    for (uint32_t i = 0; i < kBlock; ++i) {
      f[i] = far.next();
      const float played = std::tanh(1.5f * f[i]) / 1.5f;      // (the loudspeaker distorts what it is given)
      echo_part[i] = r.play(played);
      const float v = talk ? voice.next() : 0.0f;
      n[i] = echo_part[i] + v;
      echoes.push_back(echo_part[i]);
      voices.push_back(v);
    }
    CHECK(aec.process(ArrayView<const float>(f, kBlock), ArrayView<const float>(n, kBlock), ArrayView<float>(out, kBlock)));
    if (b < 1000) continue;
    for (uint32_t i = 0; i < kBlock; ++i) {
      const size_t at = size_t(b) * kBlock + i;
      if (at < latency) continue;
      const float e = echoes[at - latency], v = voices[at - latency];
      if (!talk) {
        echo_in += double(e) * e;
        echo_left += double(out[i]) * out[i];
      } else {
        talk_in += double(v) * v;
        talk_out += double(out[i]) * out[i];
      }
    }
  }
  *talk_ratio = talk_out / (talk_in > 0 ? talk_in : 1);
  return 10.0 * std::log10(echo_in / (echo_left > 1e-20 ? echo_left : 1e-20));
}

static void test_residual_suppression() {
  double talk_plain = 0, talk_suppressed = 0;
  const double plain = nonlinear_run(false, &talk_plain), suppressed = nonlinear_run(true, &talk_suppressed);
  printf("distorting loudspeaker: %.1f dB of echo gone by the filter alone, %.1f dB with the suppressor; near talker kept %.2f / %.2f of its energy\n", plain,
         suppressed, talk_plain, talk_suppressed);
  CHECK(suppressed > plain + 8.0);
  CHECK(talk_suppressed > 0.4);      // (double talk: the talker comes through, some of it suppressed with the echo)
}

static void test_noise_suppression() {
  // a talker in a noisy room: the noise in the pauses goes down by more than 10 dB, the speech stays
  EchoCanceller aec(A);
  CHECK(aec.initialize(4, 0));
  EchoCanceller::Options o;
  o.noise_suppression = true;
  aec.set_options(o);
  CHECK(aec.latency() == kBlock);
  Talker voice(5, 1.0f);
  Noise noise(9, 0.03f);
  float far[kBlock] = {}, near[kBlock], out[kBlock];
  std::vector<float> noises, voices;
  double pause_in = 0, pause_out = 0, speech_in = 0, speech_out = 0;
  for (int b = 0; b < 1500; ++b) {
    for (uint32_t i = 0; i < kBlock; ++i) {
      const uint64_t at = uint64_t(b) * kBlock + i;
      const float v = speaking(at) ? voice.next() : 0.0f, w = noise.next();
      near[i] = v + w;
      voices.push_back(v);
      noises.push_back(w);
    }
    CHECK(aec.process(ArrayView<const float>(far, kBlock), ArrayView<const float>(near, kBlock), ArrayView<float>(out, kBlock)));
    if (b < 500) continue;      // (the floor found first)
    for (uint32_t i = 0; i < kBlock; ++i) {
      const uint64_t at = uint64_t(b) * kBlock + i - kBlock;      // (the sample coming out now went in a block ago)
      if (speaking(at)) {
        // (well inside a 400 ms stretch only: the edges are neither)
        if (at % 19200 < 2400 || at % 19200 > 16800) continue;
        speech_in += double(voices[at]) * voices[at] + double(noises[at]) * noises[at];
        speech_out += double(out[i]) * out[i];
      } else {
        if (at % 19200 < 4800) continue;      // (a speech's tail, and the suppressor letting go)
        pause_in += double(noises[at]) * noises[at];
        pause_out += double(out[i]) * out[i];
      }
    }
  }
  const double reduction = 10.0 * std::log10(pause_in / (pause_out > 1e-20 ? pause_out : 1e-20)), kept = speech_out / speech_in;
  printf("noise suppression: the pauses' noise %.1f dB down, speech kept %.2f of its energy\n", reduction, kept);
  CHECK(reduction > 10.0);
  CHECK(kept > 0.6 && kept < 1.2);
}

static void test_gain_control() {
  // a quiet talker (about -40 dBFS) brought up towards -18 dBFS; a loud one (peaks past full scale) never over -0.5 dBFS
  for (int loud = 0; loud < 2; ++loud) {
    EchoCanceller aec(A);
    CHECK(aec.initialize(4, 0));
    EchoCanceller::Options o;
    o.gain_control = true;
    aec.set_options(o);
    Talker voice(13, loud ? 6.0f : 0.05f);
    Noise noise(17, 0.0005f);
    float far[kBlock] = {}, near[kBlock], out[kBlock];
    double voiced = 0, peak = 0;
    uint64_t voiced_samples = 0;
    for (int b = 0; b < 1800; ++b) {
      for (uint32_t i = 0; i < kBlock; ++i) {
        const uint64_t at = uint64_t(b) * kBlock + i;
        near[i] = (speaking(at) ? voice.next() : 0.0f) + noise.next();
      }
      CHECK(aec.process(ArrayView<const float>(far, kBlock), ArrayView<const float>(near, kBlock), ArrayView<float>(out, kBlock)));
      for (float v : out) peak = std::fabs(v) > peak ? std::fabs(v) : peak;
      if (b < 1200 || !aec.voice()) continue;
      for (float v : out) voiced += double(v) * v;
      voiced_samples += kBlock;
    }
    const double level = voiced_samples ? 10.0 * std::log10(voiced / double(voiced_samples)) : -200;
    printf("gain control (%s talker): voice at %.1f dBFS (target %.1f), gain %.1f, peak %.3f\n", loud ? "loud" : "quiet", level,
           10.0 * std::log10(EchoCanceller::kTargetLevel), aec.gain(), peak);
    CHECK(peak <= 0.95);
    if (!loud) CHECK(level > -24.0 && level < -12.0 && aec.gain() > 4.0f);
    else CHECK(aec.gain() < 1.0f);
  }
}

static void test_comfort_noise() {
  // the echo suppressed in a room with a noise floor: where the echo is cut away the output keeps that floor (comfort noise) instead of holes of
  // silence that the far end would hear as the line dropping
  EchoCanceller aec(A);
  CHECK(aec.initialize(16, 0));
  EchoCanceller::Options o;
  o.echo_suppression = true;
  aec.set_options(o);
  Talker far(61, 1.6f);
  Noise noise(67, 0.01f);
  Room r = room(900, 1.0f);
  float f[kBlock], n[kBlock], out[kBlock];
  int blocks = 0, holes = 0;
  double noise_block = 0;
  for (int b = 0; b < 2000; ++b) {
    double w_energy = 0, out_energy = 0;
    for (uint32_t i = 0; i < kBlock; ++i) {
      f[i] = far.next();
      const float w = noise.next();
      n[i] = r.play(std::tanh(1.5f * f[i]) / 1.5f) + w;
      w_energy += double(w) * w;
    }
    CHECK(aec.process(ArrayView<const float>(f, kBlock), ArrayView<const float>(n, kBlock), ArrayView<float>(out, kBlock)));
    for (float v : out) out_energy += double(v) * v;
    if (b < 1000) continue;
    noise_block = w_energy;
    ++blocks;
    holes += out_energy < 0.1 * w_energy;
  }
  printf("comfort noise: %d of %d blocks under a tenth of the room's noise (leak %.3f; noise %.2g a block)\n", holes, blocks, aec.leak(), noise_block);
  CHECK(holes * 20 < blocks);
}

int main() {
  test_sizes();
  test_silence_and_passthrough();
  test_short_echo();
  test_long_echo();
  test_double_talk();
  test_path_change();
  test_bad_input();
  test_high_pass();
  test_residual_suppression();
  test_noise_suppression();
  test_gain_control();
  test_comfort_noise();
  printf("EchoCancellerTest: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
