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

#include "IoScheduler/LatencyController.hpp"

namespace iosched {

namespace {
// How much a domain matters to the user: what it is over its target throttles only the domains below it.
constexpr uint32_t kRank[kDomainCount] = {2, 0, 1};   // read, write, other
}  // namespace

void LatencyController::initialize(uint32_t depth, const IoCostModel& model, uint32_t window_us, uint64_t now_us) {
  depth_ = depth ? (depth > 0xFFFF ? 0xFFFF : depth) : 1;
  cap_[domain_index(IoDomain::Read)] = depth_;
  cap_[domain_index(IoDomain::Write)] = depth_ * 3 / 4 ? depth_ * 3 / 4 : 1;
  cap_[domain_index(IoDomain::Other)] = depth_ / 4 ? depth_ / 4 : 1;
  for (uint32_t d = 0; d < kDomainCount; ++d) {
    target_us_[d] = model.target_us[d] ? model.target_us[d] : 1;
    __atomic_store_n(&limit_[d], cap_[d], __ATOMIC_RELAXED);
    for (uint32_t& c : counts_[d]) __atomic_store_n(&c, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&throttled_[d], 0, __ATOMIC_RELAXED);
    __atomic_store_n(&last_p90_[d], kNoData, __ATOMIC_RELAXED);
  }
  window_us_ = window_us ? window_us : 1;
  __atomic_store_n(&window_start_us_, now_us, __ATOMIC_RELAXED);
  __atomic_store_n(&adjusting_, 0, __ATOMIC_RELEASE);
}

uint32_t LatencyController::bucket_of(uint64_t latency_us, uint32_t target_us) {
  if (target_us == 0) target_us = 1;
  if (latency_us < target_us) return static_cast<uint32_t>(latency_us * 4 / target_us);   // (latency < target < 2^32: no overflow)
  const uint64_t ratio = latency_us / target_us;
  return ratio < 2 ? 4 : ratio < 4 ? 5 : ratio < 8 ? 6 : 7;
}

void LatencyController::record(IoDomain d, uint64_t latency_us, uint64_t now_us) {
  const uint32_t di = domain_index(d);
  __atomic_fetch_add(&counts_[di][bucket_of(latency_us, target_us_[di])], 1, __ATOMIC_RELAXED);
  const uint64_t start = __atomic_load_n(&window_start_us_, __ATOMIC_RELAXED);
  if (now_us - start < window_us_ || now_us < start) return;
  uint32_t expected = 0;
  if (!__atomic_compare_exchange_n(&adjusting_, &expected, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;   // another core does it
  if (now_us - __atomic_load_n(&window_start_us_, __ATOMIC_RELAXED) >= window_us_) adjust(now_us);
  __atomic_store_n(&adjusting_, 0, __ATOMIC_RELEASE);
}

void LatencyController::adjust(uint64_t now_us) {
  __atomic_store_n(&window_start_us_, now_us, __ATOMIC_RELAXED);
  __atomic_fetch_add(&windows_, 1, __ATOMIC_RELAXED);
  uint32_t samples[kDomainCount] = {}, throttled[kDomainCount] = {}, limit[kDomainCount] = {}, next[kDomainCount] = {};
  uint8_t p90[kDomainCount] = {kNoData, kNoData, kNoData};
  for (uint32_t d = 0; d < kDomainCount; ++d) {
    uint32_t counts[kBuckets];
    for (uint32_t b = 0; b < kBuckets; ++b) {
      counts[b] = __atomic_exchange_n(&counts_[d][b], 0, __ATOMIC_RELAXED);
      samples[d] += counts[b];
    }
    throttled[d] = __atomic_exchange_n(&throttled_[d], 0, __ATOMIC_RELAXED);
    limit[d] = next[d] = __atomic_load_n(&limit_[d], __ATOMIC_RELAXED);
    if (samples[d] < kMinSamples) continue;
    const uint32_t wanted = samples[d] - samples[d] / 10;   // the 90th percentile's place, counted from the fastest
    uint32_t seen = 0;
    for (uint32_t b = 0; b < kBuckets; ++b) {
      seen += counts[b];
      if (seen >= wanted) {
        p90[d] = static_cast<uint8_t>(b);
        break;
      }
    }
    __atomic_store_n(&last_p90_[d], p90[d], __ATOMIC_RELAXED);
  }

  bool over = false;
  for (uint32_t d = 0; d < kDomainCount; ++d) {
    if (p90[d] == kNoData || p90[d] < kOnTarget) continue;
    over = true;
    for (uint32_t e = 0; e < kDomainCount; ++e) {
      if (kRank[e] >= kRank[d] || (samples[e] == 0 && throttled[e] == 0)) continue;
      const uint32_t halved = limit[e] / 2 ? limit[e] / 2 : 1;
      if (halved < next[e]) next[e] = halved;
    }
    if (p90[d] == kBuckets - 1) {
      const uint32_t reduced = limit[d] * 3 / 4 ? limit[d] * 3 / 4 : 1;
      if (reduced < next[d]) next[d] = reduced;
    }
  }
  if (!over) {
    for (uint32_t d = 0; d < kDomainCount; ++d) {
      if (throttled[d] == 0 && samples[d] >= kMinSamples) continue;   // it had what it needed (and was not idle)
      const uint32_t step = limit[d] / 4 ? limit[d] / 4 : 1;
      next[d] = limit[d] + step < cap_[d] ? limit[d] + step : cap_[d];
    }
  }
  for (uint32_t d = 0; d < kDomainCount; ++d) {
    if (next[d] < limit[d]) __atomic_fetch_add(&shrinks_, 1, __ATOMIC_RELAXED);
    if (next[d] > limit[d]) __atomic_fetch_add(&grows_, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&limit_[d], next[d], __ATOMIC_RELAXED);
  }
}

LatencyControllerStatistics LatencyController::statistics() const {
  LatencyControllerStatistics s;
  for (uint32_t d = 0; d < kDomainCount; ++d) {
    s.limit[d] = __atomic_load_n(&limit_[d], __ATOMIC_RELAXED);
    s.last_p90_bucket[d] = __atomic_load_n(&last_p90_[d], __ATOMIC_RELAXED);
  }
  s.windows = __atomic_load_n(&windows_, __ATOMIC_RELAXED);
  s.shrinks = __atomic_load_n(&shrinks_, __ATOMIC_RELAXED);
  s.grows = __atomic_load_n(&grows_, __ATOMIC_RELAXED);
  return s;
}

}  // namespace iosched
