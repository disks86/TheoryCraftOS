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

#include "Hardware/AudioDevice/Software/SoftwareAudioCapture.hpp"

namespace hardware {

bool SoftwareAudioCapture::start(allocators::IAllocator& allocator) {
  kernel::ScopedMutex guard(mutex_);
  if (!ring_.allocate(allocator, kRingBytes)) return false;
  last_us_ = clock_.now_us();
  fraction_ = 0;
  running_ = true;
  return true;
}

void SoftwareAudioCapture::stop(allocators::IAllocator& allocator) {
  kernel::ScopedMutex guard(mutex_);
  running_ = false;
  ring_.release(allocator);
}

// The ring is silent to begin with, so recording is just time passing.
void SoftwareAudioCapture::refresh() {
  if (!running_) return;
  const uint64_t now = clock_.now_us();
  const uint64_t elapsed = now > last_us_ ? now - last_us_ : 0;
  last_us_ = now;
  const uint64_t scaled = elapsed * kAudioSampleRate + fraction_;
  fraction_ = scaled % 1000000;
  ring_.advance_by(scaled / 1000000 * kAudioFrameSize);
}

}  // namespace hardware
