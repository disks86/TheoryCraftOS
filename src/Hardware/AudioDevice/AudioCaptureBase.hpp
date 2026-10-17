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

#pragma once
#include "Hardware/AudioDevice/CaptureRing.hpp"
#include "Hardware/AudioDevice/IAudioCapture.hpp"
#include "Kernel/Sync.hpp"

namespace hardware {

// The common part of the capture drivers: a CaptureRing kept up to date from the
// hardware, a lock, input gain and mute. A driver either reports the DMA position
// (hardware_position()) or overrides refresh() to feed the ring itself, and
// either applies gain in its hardware (apply_gain()) or leaves it to read(),
// which then scales the samples.
class RingAudioCapture : public IAudioCapture {
 public:
  uint32_t read(ByteView out) override {
    kernel::ScopedMutex guard(mutex_);
    refresh();
    const uint32_t bytes = ring_.read(out);
    if (bytes > 0 && !hardware_gain() && (muted_ || gain_ < kAudioMaxVolume)) {
      for (uint32_t i = 0; i + 1 < bytes; i += kAudioBytesPerSample) {
        const int32_t sample = muted_ ? 0 : static_cast<int16_t>(out[i] | (out[i + 1] << 8)) * gain_ / kAudioMaxVolume;
        out[i] = static_cast<uint8_t>(sample);
        out[i + 1] = static_cast<uint8_t>(sample >> 8);
      }
    }
    return bytes;
  }
  uint32_t available_bytes() override {
    kernel::ScopedMutex guard(mutex_);
    refresh();
    return ring_.available();
  }
  void flush() override {
    kernel::ScopedMutex guard(mutex_);
    refresh();
    ring_.flush();
  }
  void set_gain(uint8_t gain) override {
    kernel::ScopedMutex guard(mutex_);
    gain_ = gain < kAudioMaxVolume ? gain : kAudioMaxVolume;
    apply_gain();
  }
  uint8_t gain() const override { return gain_; }
  void set_muted(bool muted) override {
    kernel::ScopedMutex guard(mutex_);
    muted_ = muted;
    apply_gain();
  }
  bool muted() const override { return muted_; }
  AudioCaptureStatistics statistics() override {
    kernel::ScopedMutex guard(mutex_);
    refresh();
    AudioCaptureStatistics stats;
    stats.frames_captured = ring_.frames_captured();
    stats.frames_read = ring_.frames_read();
    stats.frames_lost = ring_.frames_lost();
    stats.overruns = ring_.overruns();
    return stats;
  }

 protected:
  // Byte offset into the ring the hardware is writing at now. Called with the lock held.
  virtual uint32_t hardware_position() { return 0; }
  // Brings the ring up to date with the hardware.
  virtual void refresh() {
    if (running_) ring_.advance(hardware_position());
  }
  // True when apply_gain() really changes the level in hardware.
  virtual bool hardware_gain() const { return false; }
  // Pushes gain_/muted_ to the hardware. Called with the lock held.
  virtual void apply_gain() {}

  CaptureRing ring_;
  kernel::Mutex mutex_;
  bool running_ = false;
  uint8_t gain_ = kAudioMaxVolume;
  bool muted_ = false;
};

}  // namespace hardware
