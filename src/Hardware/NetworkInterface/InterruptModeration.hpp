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

// Adaptive interrupt moderation, after igb_update_itr / e1000_update_itr: the frames and bytes an interrupt vector saw since it was last
// rearmed put it in one of three classes - lowest latency (small, sparse frames: 70000 interrupts a second), low latency (20000) and bulk
// (4000, for streams of full frames) - and the hardware's throttle (ITR, EITR) is set from the class. As with igb and e1000e's default
// ("conservative", InterruptThrottleRate=3) the lowest-latency class is not used: from it full frames would never reach the bulk class,
// so a stream would run at 70000 interrupts a second.
#pragma once
#include <stdint.h>

namespace hardware {

enum class InterruptLatency : uint8_t { Lowest, Low, Bulk };

class InterruptModerator {
 public:
  void account(uint32_t frames, uint32_t bytes) {
    frames_ += frames;
    bytes_ = bytes > ~uint32_t{0} - bytes_ ? ~uint32_t{0} : bytes_ + bytes;
  }
  // Ends an interval (the vector is rearmed): true if the class changed, so the throttle is to be written again.
  bool update() {
    InterruptLatency next = classify(latency_, frames_, bytes_);
    if (next == InterruptLatency::Lowest) next = InterruptLatency::Low;  // (conservative)
    frames_ = bytes_ = 0;
    if (next == latency_) return false;
    latency_ = next;
    return true;
  }
  InterruptLatency latency() const { return latency_; }
  uint32_t interrupts_per_second() const { return rate(latency_); }

  static uint32_t rate(InterruptLatency latency) { return latency == InterruptLatency::Lowest ? 70000 : latency == InterruptLatency::Low ? 20000 : 4000; }
  // The interval Intel's ITR and EITR registers take, in 256 ns units (e1000e: ITR = 10^9 / (rate * 256); igb/igc: 56, 196, 980).
  static uint32_t intel_interval(InterruptLatency latency) {
    return latency == InterruptLatency::Lowest ? 56 : latency == InterruptLatency::Low ? 196 : 980;
  }

  static InterruptLatency classify(InterruptLatency current, uint32_t frames, uint32_t bytes) {
    if (frames == 0) return current;
    const uint32_t per_frame = bytes / frames;
    switch (current) {
      case InterruptLatency::Lowest:
        if (per_frame > 8000) return InterruptLatency::Bulk;  // segmentation offload: few, huge "frames"
        if (frames < 5 && bytes > 512) return InterruptLatency::Low;
        return current;
      case InterruptLatency::Low:
        if (bytes > 10000) {
          if (per_frame > 8000 || frames < 10 || per_frame > 1200) return InterruptLatency::Bulk;
          if (frames > 35) return InterruptLatency::Lowest;
          return current;
        }
        if (per_frame > 2000) return InterruptLatency::Bulk;
        if (frames <= 2 && bytes < 512) return InterruptLatency::Lowest;
        return current;
      case InterruptLatency::Bulk:
        if (bytes > 25000) return frames > 35 ? InterruptLatency::Low : current;
        if (bytes < 1500) return InterruptLatency::Low;
        return current;
    }
    return current;
  }

 private:
  InterruptLatency latency_ = InterruptLatency::Low;
  uint32_t frames_ = 0, bytes_ = 0;
};

}  // namespace hardware
