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
#include "Allocators/IAllocator.hpp"
#include "Hardware/AudioDevice/AudioDeviceBase.hpp"
#include "Hardware/AudioDevice/Software/SoftwareAudioCapture.hpp"
#include "Hardware/Clock/IClock.hpp"

namespace hardware {

// An audio device with no hardware behind it: it accepts samples and throws them
// away at the rate real hardware would play them, timed by a clock (and its input hears silence). It is what
// the system falls back to on a machine without a supported sound card, so
// programs that play audio still run (silently). Volume and mute are remembered.
class SoftwareAudioDevice final : public RingAudioDevice {
 public:
  static constexpr uint32_t kRingBytes = 64 * 1024;

  SoftwareAudioDevice(const IClock& clock, allocators::IAllocator& allocator)
      : clock_(clock), allocator_(allocator), capture_(clock) {}

  const char* name() const override { return "Software audio (silent)"; }
  bool initialize() override;
  void shutdown() override;
  IAudioCapture* capture() override { return capture_started_ ? &capture_ : nullptr; }

 private:
  void refresh() override;
  void apply_volume() override {}

  const IClock& clock_;
  allocators::IAllocator& allocator_;
  SoftwareAudioCapture capture_;
  bool capture_started_ = false;
  uint64_t last_us_ = 0;
  uint64_t fraction_ = 0;   // microsecond-samples carried between calls (not yet a whole frame)
};

}  // namespace hardware
