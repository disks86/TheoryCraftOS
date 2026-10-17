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

#include "AudioDemo.hpp"
#include "Kernel/Delay.hpp"
#include "console.hpp"

namespace {
constexpr uint32_t kToneFrames = hardware::kAudioSampleRate / 3;
constexpr uint32_t kPeriodFrames = hardware::kAudioSampleRate / 440;  // one wave of 440 Hz
constexpr int32_t kAmplitude = 6000;

void report(const char* what, bool ok) {
  console::puts("  ");
  console::puts(what);
  console::puts(ok ? ": ok\n" : ": FAILED\n");
}

// Stereo frame `n` of the triangle wave.
void put_frame(uint8_t* out, uint32_t n) {
  const uint32_t phase = n % kPeriodFrames, half = kPeriodFrames / 2;
  const int32_t rising = phase < half ? static_cast<int32_t>(phase) : static_cast<int32_t>(kPeriodFrames - phase);
  const int32_t sample = (rising * 2 * kAmplitude / static_cast<int32_t>(half)) - kAmplitude;
  for (uint32_t channel = 0; channel < hardware::kAudioChannels; ++channel) {
    out[2 * channel] = static_cast<uint8_t>(sample);
    out[2 * channel + 1] = static_cast<uint8_t>(sample >> 8);
  }
}
}  // namespace

bool run_audio_demo(hardware::IAudioDevice& device) {
  bool all_ok = true;
  const hardware::AudioStatistics before = device.statistics();

  // Feed the tone in small pieces, as a program would, until the device has taken all of it.
  uint32_t frames_sent = 0;
  uint64_t waited_us = 0;
  while (frames_sent < kToneFrames && waited_us < 3000000) {
    uint8_t chunk[hardware::kAudioFrameSize * 256];
    const uint32_t frames = kToneFrames - frames_sent < 256 ? kToneFrames - frames_sent : 256;
    for (uint32_t i = 0; i < frames; ++i) put_frame(chunk + i * hardware::kAudioFrameSize, frames_sent + i);
    const uint32_t taken = device.write(ConstByteView(chunk).first(frames * hardware::kAudioFrameSize));
    frames_sent += taken / hardware::kAudioFrameSize;
    if (taken < frames * hardware::kAudioFrameSize) {  // the device is full: wait for it to play some
      hardware::delay_us(5000);
      waited_us += 5000;
    }
  }
  const bool accepted = frames_sent == kToneFrames;
  report("tone accepted", accepted);
  all_ok &= accepted;

  // Let it play out.
  waited_us = 0;
  while (device.queued_bytes() > 0 && waited_us < 2000000) {
    hardware::delay_us(10000);
    waited_us += 10000;
  }
  const hardware::AudioStatistics after = device.statistics();
  const bool played = device.queued_bytes() == 0 && after.frames_played - before.frames_played == kToneFrames &&
                      after.frames_queued - before.frames_queued == kToneFrames;
  report("tone played and counted", played);
  all_ok &= played;

  // Volume and mute are remembered.
  const uint8_t old_volume = device.volume();
  device.set_volume(37);
  device.set_muted(true);
  const bool controls = device.volume() == 37 && device.muted();
  device.set_muted(false);
  device.set_volume(old_volume);
  report("volume and mute", controls && !device.muted() && device.volume() == old_volume);
  all_ok &= controls;
  return all_ok;
}

namespace {
// Stereo frame `n` of a triangle wave of `period` frames, at `amplitude`.
void tone_frame(uint8_t* out, uint32_t n, uint32_t period, int32_t amplitude) {
  const uint32_t phase = n % period, half = period / 2;
  const int32_t rising = phase < half ? static_cast<int32_t>(phase) : static_cast<int32_t>(period - phase);
  const int32_t sample = (rising * 2 * amplitude / static_cast<int32_t>(half)) - amplitude;
  for (uint32_t channel = 0; channel < hardware::kAudioChannels; ++channel) {
    out[2 * channel] = static_cast<uint8_t>(sample);
    out[2 * channel + 1] = static_cast<uint8_t>(sample >> 8);
  }
}
}  // namespace

bool run_mixer_demo(audio::AudioMixer& mixer) {
  bool all_ok = true;
  const audio::MixerStatistics before = mixer.statistics();
  SharedPointer<audio::AudioStream> low = mixer.open_stream(), high = mixer.open_stream();
  const bool opened = low && high && mixer.stream_count() >= 2;
  report("two streams opened", opened);
  if (!opened) return false;

  const uint32_t periods[2] = {hardware::kAudioSampleRate / 440, hardware::kAudioSampleRate / 660};
  audio::AudioStream* streams[2] = {low.get(), high.get()};
  uint32_t sent[2] = {0, 0};
  uint64_t waited_us = 0;
  while ((sent[0] < kToneFrames || sent[1] < kToneFrames) && waited_us < 3000000) {
    bool blocked = true;
    for (int s = 0; s < 2; ++s) {
      if (sent[s] >= kToneFrames) continue;
      uint8_t chunk[hardware::kAudioFrameSize * 256];
      const uint32_t frames = kToneFrames - sent[s] < 256 ? kToneFrames - sent[s] : 256;
      for (uint32_t i = 0; i < frames; ++i) tone_frame(chunk + i * hardware::kAudioFrameSize, sent[s] + i, periods[s], 4000);
      const uint32_t taken = streams[s]->write(ConstByteView(chunk).first(frames * hardware::kAudioFrameSize));
      sent[s] += taken / hardware::kAudioFrameSize;
      if (taken > 0) blocked = false;
    }
    if (blocked) {
      hardware::delay_us(5000);
      waited_us += 5000;
    }
  }
  const bool accepted = sent[0] == kToneFrames && sent[1] == kToneFrames;
  report("both tones accepted", accepted);
  all_ok &= accepted;
  // A third sound in another format: 24 kHz mono, 8-bit, panned to the left.
  SharedPointer<audio::AudioStream> side = mixer.open_stream(audio::StreamFormat{24000, 1, audio::SampleType::U8});
  bool side_ok = static_cast<bool>(side);
  if (side) {
    side->set_pan(audio::kPanLeft);
    uint8_t note[240];
    for (uint32_t i = 0; i < sizeof note; ++i) note[i] = (i / 27) % 2 ? 128 + 40 : 128 - 40;
    uint32_t written = 0;
    for (int tries = 0; written < sizeof note && tries < 100; ++tries) {
      written += side->write(ConstByteView(note).dropped(written));
      if (written < sizeof note) hardware::delay_us(5000);
    }
    side_ok = written == sizeof note;
    side->close();
  }
  report("24 kHz mono stream resampled and panned", side_ok);
  all_ok &= side_ok;
  low->close();
  high->close();

  // The mixer thread drains the streams and the device plays the sum.
  waited_us = 0;
  while ((mixer.stream_count() > 0 || mixer.device().queued_bytes() > 0) && waited_us < 3000000) {
    hardware::delay_us(10000);
    waited_us += 10000;
  }
  const audio::MixerStatistics after = mixer.statistics();
  const bool mixed = after.frames_mixed - before.frames_mixed >= kToneFrames &&
                     after.streams_opened - before.streams_opened == 3 && mixer.stream_count() == 0;
  report("tones mixed, played and streams released", mixed);
  all_ok &= mixed;
  return all_ok;
}

bool run_capture_demo(audio::AudioCaptureHub& hub) {
  bool all_ok = true;
  SharedPointer<audio::CaptureStream> a = hub.open_stream(), b = hub.open_stream();
  const bool opened = a && b;
  report("two capture streams opened", opened);
  if (!opened) return false;

  constexpr uint64_t kListenUs = 250000;
  hardware::delay_us(kListenUs);
  static uint8_t buffer[hardware::kAudioSampleRate * hardware::kAudioFrameSize / 4];  // up to 250 ms
  const uint32_t got_a = a->read(ByteView(buffer));
  int32_t peak = 0;
  for (uint32_t i = 0; i + 1 < got_a; i += 2) {
    int32_t sample = static_cast<int16_t>(buffer[i] | (buffer[i + 1] << 8));
    if (sample < 0) sample = -sample;
    if (sample > peak) peak = sample;
  }
  const uint32_t got_b = b->read(ByteView(buffer));
  const uint32_t expected = static_cast<uint32_t>(kListenUs * hardware::kAudioSampleRate / 1000000) * hardware::kAudioFrameSize;
  // The hub delivers every few milliseconds, and delay_us may run long: accept a wide band.
  const bool amount = got_a > expected / 2 && got_a <= sizeof buffer && got_b > expected / 2;
  report("about a quarter second recorded by each stream", amount);
  all_ok &= amount;
  console::puts("  loudest sample heard: ");
  console::put_dec(static_cast<uint64_t>(peak));
  console::puts("\n");

  a->set_gain(50);
  a->set_muted(true);
  const bool controls = a->gain() == 50 && a->muted();
  a->set_muted(false);
  report("gain and mute", controls && !a->muted());
  all_ok &= controls;

  a->close();
  b->close();
  hardware::delay_us(20000);
  const bool released = hub.stream_count() == 0;
  report("streams released", released);
  all_ok &= released;
  return all_ok;
}
