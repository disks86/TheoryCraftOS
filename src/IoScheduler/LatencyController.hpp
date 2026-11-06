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

// The depth controller of the I/O scheduler: how many requests of each domain (read, write, other) may be on the device at once. A flash
// drive queues internally, so a deep queue of writes makes every read behind it slow while throughput hardly grows. Like Linux's Kyber, the
// completion latencies of each domain go into a small histogram relative to its target (IoCostModel::target_us) and, once per window, the
// limits are set from the 90th percentiles:
//
// - a domain over its target halves the limits of the domains less important than it (read > other > write) that had traffic;
// - a domain far over its target (8x) also loses a quarter of its own limit: the device is overloaded by it alone;
// - when every domain is within its target, a domain whose requests had to wait for a slot, or that was idle, gets a quarter more, up to its
//   cap (read: the whole depth, write: three quarters of it, other: a quarter).
//
// Lock-free: completions on any core record with atomics; whoever finds the window over runs the adjustment (one at a time, by a flag) and
// publishes the new limits with atomic stores. A limit is never below 1, so no domain stops.
#pragma once
#include <stdint.h>
#include "IoScheduler/IoTypes.hpp"

namespace iosched {

struct LatencyControllerStatistics {
  uint32_t limit[kDomainCount] = {};
  uint8_t last_p90_bucket[kDomainCount] = {};   // of the last window with enough samples (LatencyController::kNoData if none)
  uint64_t windows = 0, shrinks = 0, grows = 0;
};

class LatencyController {
 public:
  static constexpr uint32_t kBuckets = 8;      // 0-3: quarters of the target, 4: 1-2x, 5: 2-4x, 6: 4-8x, 7: 8x and more
  static constexpr uint32_t kOnTarget = 4;     // the first bucket over the target
  static constexpr uint32_t kMinSamples = 8;   // fewer completions in a window say nothing about the domain
  static constexpr uint8_t kNoData = 0xFF;

  void initialize(uint32_t depth, const IoCostModel& model, uint32_t window_us, uint64_t now_us);

  uint32_t depth() const { return depth_; }
  uint32_t limit(IoDomain d) const { return __atomic_load_n(&limit_[domain_index(d)], __ATOMIC_RELAXED); }
  uint32_t cap(IoDomain d) const { return cap_[domain_index(d)]; }
  uint32_t target_us(IoDomain d) const { return target_us_[domain_index(d)]; }

  // A request of the domain took `latency_us` on the device. May end the window and adjust the limits.
  void record(IoDomain d, uint64_t latency_us, uint64_t now_us);
  // A request of the domain waited because the domain had no slot free.
  void note_throttled(IoDomain d) { __atomic_fetch_add(&throttled_[domain_index(d)], 1, __ATOMIC_RELAXED); }
  // Ends the current window at once (tests, and record()).
  void adjust(uint64_t now_us);

  static uint32_t bucket_of(uint64_t latency_us, uint32_t target_us);
  LatencyControllerStatistics statistics() const;

 private:
  uint32_t depth_ = 1;
  uint32_t cap_[kDomainCount] = {1, 1, 1};
  uint32_t target_us_[kDomainCount] = {1, 1, 1};
  uint32_t window_us_ = 100000;
  uint32_t limit_[kDomainCount] = {1, 1, 1};              // atomic
  uint32_t counts_[kDomainCount][kBuckets] = {};          // atomic
  uint32_t throttled_[kDomainCount] = {};                 // atomic
  uint64_t window_start_us_ = 0;                          // atomic
  uint32_t adjusting_ = 0;                                // atomic: 1 while one core adjusts
  uint8_t last_p90_[kDomainCount] = {kNoData, kNoData, kNoData};   // atomic
  uint64_t windows_ = 0, shrinks_ = 0, grows_ = 0;        // atomic
};

}  // namespace iosched
