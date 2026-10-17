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

// Host-side tests for the audio player (transport over the mixer), PCM sources, WAV parsing and the
// level meter scale.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "Audio/AudioPlayer.hpp"
#include "Audio/WavFile.hpp"

using namespace hardware;
using namespace audio;

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

// Counts what it is given and plays it "instantly" when told to.
struct FakeDevice final : IAudioDevice {
  uint32_t queued = 0; uint64_t total = 0; int16_t first_sample = 0, last_sample = 0;
  uint8_t vol = 80; bool mute = false;
  const char* name() const override { return "fake audio"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint32_t write(ConstByteView pcm) override {
    uint32_t n = pcm.size() < 16384 - queued ? (uint32_t)pcm.size() : 16384 - queued;
    n -= n % kAudioFrameSize;
    if (n && total == 0) first_sample = (int16_t)(pcm[0] | (pcm[1] << 8));
    if (n) last_sample = (int16_t)(pcm[n - 4] | (pcm[n - 3] << 8));
    queued += n; total += n;
    return n;
  }
  uint32_t queued_bytes() override { return queued; }
  uint32_t free_bytes() override { return 16384 - queued; }
  void play_all() { queued = 0; }
  void set_volume(uint8_t v) override { vol = v; }
  uint8_t volume() const override { return vol; }
  void set_muted(bool m) override { mute = m; }
  bool muted() const override { return mute; }
  AudioStatistics statistics() override { return {}; }
};

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct Notified { int count = 0; static void fn(void* c) { ++static_cast<Notified*>(c)->count; } };

// Runs the player, the mixer and the "hardware" for a while.
static void run(AudioPlayer& player, AudioMixer& mixer, FakeDevice& dev, int rounds) {
  for (int i = 0; i < rounds; ++i) {
    player.update();
    mixer.pump();
    dev.play_all();
  }
}

static int16_t g_tone[48000 * 2];  // one second, stereo

static void test_sources_and_wav() {
  for (int i = 0; i < 48000; ++i) g_tone[2 * i] = g_tone[2 * i + 1] = (int16_t)(i % 1000);
  MemoryPcmSource source(ConstByteView((const uint8_t*)g_tone, sizeof g_tone), StreamFormat());
  CHECK(source.frame_count() == 48000);
  uint8_t out[40];
  CHECK(source.read(10, ByteView(out, 40)) == 40);
  CHECK(out[0] == 10 && out[4] == 11);
  CHECK(source.read(47999, ByteView(out, 40)) == 4);   // only one frame is left
  CHECK(source.read(48000, ByteView(out, 40)) == 0);
  CHECK(source.read(0, ByteView(out, 3)) == 0);        // not even a whole frame fits

  // A minimal WAV: 8-bit mono 8 kHz, 4 samples, with an extra chunk in front and a padded odd-sized chunk.
  uint8_t wav[80]; uint32_t n = 0;
  auto put = [&](const void* d, uint32_t len) { memcpy(wav + n, d, len); n += len; };
  auto put32 = [&](uint32_t v) { put(&v, 4); };
  auto put16 = [&](uint16_t v) { put(&v, 2); };
  put("RIFF", 4); put32(0); put("WAVE", 4);
  put("LIST", 4); put32(3); put("abc", 3); put("\0", 1);          // odd length, padded
  put("fmt ", 4); put32(16); put16(1); put16(1); put32(8000); put32(8000); put16(1); put16(8);
  put("data", 4); put32(4); const uint8_t samples[4] = {128, 255, 0, 128}; put(samples, 4);
  StreamFormat f; ConstByteView data;
  CHECK(WavFile::parse(ConstByteView(wav, n), &f, &data));
  CHECK(f.sample_rate == 8000 && f.channels == 1 && f.type == SampleType::U8 && data.size() == 4 && data[1] == 255);
  // A data chunk that claims too much is cut to the file.
  uint8_t cut[80]; memcpy(cut, wav, n);
  cut[n - 8] = 0xFF;  // data size low byte
  CHECK(WavFile::parse(ConstByteView(cut, n), &f, &data) && data.size() == 4);
  // Not WAVs.
  CHECK(!WavFile::parse(ConstByteView(wav, 11), &f, &data));
  uint8_t bad[80]; memcpy(bad, wav, n);
  bad[0] = 'X';
  CHECK(!WavFile::parse(ConstByteView(bad, n), &f, &data));
  memcpy(bad, wav, n);
  bad[12 + 8 + 4 + 8] = 3;   // the fmt chunk's format tag: IEEE float, which we do not play
  CHECK(!WavFile::parse(ConstByteView(bad, n), &f, &data));
  CHECK(!WavFile::parse(ConstByteView(wav, 12 + 8 + 4), &f, &data));  // no fmt or data

  // The meter scale: silence, -6 dB steps, full scale.
  CHECK(peak_to_percent(0) == 0 && peak_to_percent(32768) == 100 && peak_to_percent(32767) >= 99);
  CHECK(peak_to_percent(16384) >= 89 && peak_to_percent(16384) <= 91);
  CHECK(peak_to_percent(8192) >= 79 && peak_to_percent(8192) <= 81);
  CHECK(peak_to_percent(10) == 0 && peak_to_percent(1000) > peak_to_percent(100));
}

static void test_player() {
  TestAllocator a; FakeDevice dev;
  AudioMixer mixer(dev, a);
  CHECK(mixer.initialize());
  {
    AudioPlayer player(mixer, a);
    Notified notified;
    player.on_state_changed(Notified::fn, &notified);
    CHECK(player.state() == PlayerState::Empty && !player.play());
    MemoryPcmSource source(ConstByteView((const uint8_t*)g_tone, sizeof g_tone), StreamFormat());
    CHECK(player.open(&source));
    CHECK(player.state() == PlayerState::Stopped && player.duration() == 48000 && player.sample_rate() == 48000 && notified.count == 1);
    CHECK(mixer.stream_count() == 1);

    CHECK(player.play() && player.playing());
    run(player, mixer, dev, 3);
    CHECK(dev.total > 0 && dev.first_sample == 0);
    const uint64_t pos = player.position();
    CHECK(pos > 0 && pos < 48000);

    // Pause keeps the place and stops feeding.
    player.pause();
    CHECK(player.state() == PlayerState::Paused);
    const uint64_t paused_at = player.position();
    const uint64_t fed = dev.total;
    run(player, mixer, dev, 5);
    CHECK(dev.total == fed && player.position() == paused_at && paused_at <= pos + 1);
    // Resume continues from the same frame: the next sample is the one after the paused position.
    player.play();
    dev.total = 0;
    run(player, mixer, dev, 1);
    CHECK(dev.first_sample == (int16_t)(paused_at % 1000));

    // Seek while playing: the next audio is from there.
    player.seek(30500);
    dev.total = 0;
    run(player, mixer, dev, 1);
    CHECK(dev.first_sample == 500);
    // Seek past the end is clamped.
    player.seek(999999);
    CHECK(player.position() <= player.duration());

    // Play through to the end: the player stops by itself at the end.
    player.seek(40000);
    for (int i = 0; i < 40 && player.playing(); ++i) run(player, mixer, dev, 1);
    CHECK(player.state() == PlayerState::Stopped && player.position() == 48000);
    CHECK(dev.last_sample == 999);  // the last frame was frame 47999: 47999 % 1000
    // Playing again starts from the beginning.
    CHECK(player.play());
    run(player, mixer, dev, 1);
    CHECK(player.position() < 48000 && dev.first_sample >= 0);
    player.stop();
    CHECK(player.state() == PlayerState::Stopped && player.position() == 0);

    // Looping goes round again instead of stopping.
    player.set_looping(true);
    player.seek(47000);
    dev.total = 0;
    player.play();
    for (int i = 0; i < 80; ++i) run(player, mixer, dev, 1);
    CHECK(player.playing() && dev.total > 48000 * 4);  // more than a second came out of a 1000-frame tail
    player.stop();
    player.set_looping(false);

    // Volume, pan and mute are the stream's; the peak reports what was played.
    player.set_volume(50);
    player.set_pan(-100);
    player.set_muted(false);
    CHECK(player.volume() == 50 && player.pan() == -100);
    player.take_peak();
    player.seek(0);
    player.play();
    run(player, mixer, dev, 2);
    const uint32_t peak = player.take_peak();
    CHECK(peak > 0 && peak <= 32767 && player.take_peak() == 0);
    player.set_muted(true);
    run(player, mixer, dev, 2);
    CHECK(player.take_peak() == 0);
    player.stop();

    // A different format: 24 kHz mono 8-bit plays through the stream's conversion, and the position is in its own frames.
    static uint8_t mono[24000];
    for (uint32_t i = 0; i < sizeof mono; ++i) mono[i] = (uint8_t)(128 + (i % 50));
    MemoryPcmSource voice(ConstByteView(mono, sizeof mono), StreamFormat{24000, 1, SampleType::U8});
    CHECK(player.open(&voice));
    CHECK(player.duration() == 24000 && player.sample_rate() == 24000);
    dev.total = 0;
    player.play();
    for (int i = 0; i < 60 && player.playing(); ++i) run(player, mixer, dev, 1);
    CHECK(player.state() == PlayerState::Stopped && dev.total >= 2 * 24000 * 4 - 400 && dev.total <= 2 * 24000 * 4 + 400);

    player.close();
    CHECK(player.state() == PlayerState::Empty && !player.play());
    mixer.pump();
    CHECK(mixer.stream_count() == 0);
    CHECK(notified.count >= 8);
  }
  CHECK(mixer.stream_count() == 0);
}

int main() {
  test_sources_and_wav();
  test_player();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("audio player tests passed\n");
  return 0;
}
