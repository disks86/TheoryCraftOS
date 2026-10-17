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
#include "Hardware/AudioDevice/AudioCaptureBase.hpp"
#include "Hardware/Clock/IClock.hpp"

namespace hardware {

// A microphone that hears nothing: it records silence at the rate real hardware
// would, timed by a clock. The capture side of SoftwareAudioDevice.
class SoftwareAudioCapture final : public RingAudioCapture {
 public:
  static constexpr uint32_t kRingBytes = 64 * 1024;

  explicit SoftwareAudioCapture(const IClock& clock) : clock_(clock) {}

  const char* name() const override { return "Software audio input (silent)"; }
  bool start(allocators::IAllocator& allocator);
  void stop(allocators::IAllocator& allocator);

 private:
  void refresh() override;

  const IClock& clock_;
  uint64_t last_us_ = 0;
  uint64_t fraction_ = 0;  // microsecond-samples not yet a whole frame
};

}  // namespace hardware
