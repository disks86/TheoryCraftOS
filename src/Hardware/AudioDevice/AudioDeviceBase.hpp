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
#include "Hardware/AudioDevice/IAudioDevice.hpp"
#include "Hardware/AudioDevice/PlaybackRing.hpp"
#include "Kernel/Sync.hpp"

namespace hardware {

// Volume, mute and locking shared by the audio drivers. A driver implements
// apply_volume() to push volume_/muted_ to its hardware; it is called with the
// lock held (and once from the driver's initialize()).
class AudioDeviceBase : public IAudioDevice {
 public:
  void set_volume(uint8_t volume) override {
    kernel::ScopedMutex guard(mutex_);
    volume_ = volume < kAudioMaxVolume ? volume : kAudioMaxVolume;
    apply_volume();
  }
  uint8_t volume() const override { return volume_; }
  void set_muted(bool muted) override {
    kernel::ScopedMutex guard(mutex_);
    muted_ = muted;
    apply_volume();
  }
  bool muted() const override { return muted_; }

 protected:
  virtual void apply_volume() = 0;

  kernel::Mutex mutex_;
  uint8_t volume_ = 80;
  bool muted_ = false;
};

// An audio device that plays from a PlaybackRing the hardware reads cyclically.
// A driver supplies the hardware's current read position; everything else
// (queueing, underrun accounting, statistics) is here.
class RingAudioDevice : public AudioDeviceBase {
 public:
  uint32_t write(ConstByteView pcm) override {
    kernel::ScopedMutex guard(mutex_);
    if (!running_) {
      ++write_failures_;
      return 0;
    }
    refresh();
    const uint32_t taken = ring_.write(pcm);
    if (taken == 0 && !pcm.empty()) ++write_failures_;
    return taken;
  }
  uint32_t queued_bytes() override {
    kernel::ScopedMutex guard(mutex_);
    refresh();
    return ring_.queued();
  }
  uint32_t free_bytes() override {
    kernel::ScopedMutex guard(mutex_);
    if (!running_) return 0;
    refresh();
    return ring_.free();
  }
  AudioStatistics statistics() override {
    kernel::ScopedMutex guard(mutex_);
    refresh();
    AudioStatistics stats;
    stats.frames_played = ring_.frames_played();
    stats.frames_queued = ring_.frames_queued();
    stats.underruns = ring_.underruns();
    stats.write_failures = write_failures_;
    return stats;
  }

 protected:
  // Byte offset into the ring the hardware is reading at now. Called with the lock held.
  virtual uint32_t hardware_position() { return 0; }
  // Brings the ring up to date with the hardware. A driver with no position to
  // read overrides this and uses ring_.advance_by() instead.
  virtual void refresh() {
    if (running_) ring_.advance(hardware_position());
  }

  PlaybackRing ring_;
  bool running_ = false;
  uint64_t write_failures_ = 0;
};

}  // namespace hardware
