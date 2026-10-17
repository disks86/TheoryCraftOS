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
constexpr uint32_t kPeriodFrames = hardware::kAudioSampleRate / 440;  // one wave
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
