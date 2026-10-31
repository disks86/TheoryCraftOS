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

#include "Hardware/WirelessInterface/RateScaler.hpp"

namespace hardware {

constexpr uint32_t WifiRateScaler::kRatesKbps[];
constexpr uint8_t WifiRateScaler::kPlcp[];

namespace {
constexpr uint32_t kOne = 1024;
constexpr uint32_t kPoor = 640;               // 62 %: below this the rate is too high
constexpr uint32_t kGood = 940;               // 92 %: above this the next rate may be tried
constexpr uint32_t kProbeKeeps = 800;         // 78 %: a probe that does at least this well stays
constexpr uint32_t kMaxBackoff = 8;
constexpr uint32_t kForgetAfterWindows = 12;     // the channel changes: what was measured long ago says little
}  // namespace

void WifiRateScaler::start(uint32_t signal_percent) {
  current_ = signal_percent >= 75 ? 6 : signal_percent >= 60 ? 5 : signal_percent >= 45 ? 4 : signal_percent >= 30 ? 2 : 0;
  window_frames_ = window_ok_ = window_attempts_ = 0;
  window_started_ = false;
  probing_ = false;
  changed_ = true;
  for (uint32_t i = 0; i < kRateCount; ++i) {
    chance_[i] = 0;
    known_[i] = false;
    hold_[i] = 0;
    age_[i] = 0;
    backoff_[i] = 1;
  }
}

uint32_t WifiRateScaler::index_of_plcp(uint8_t plcp) {
  for (uint32_t i = 0; i < kRateCount; ++i)
    if (kPlcp[i] == plcp) return i;
  return kRateCount;
}

void WifiRateScaler::fill_table(uint8_t indices[kTableSize]) const {
  uint32_t at = 0;
  for (int32_t rate = static_cast<int32_t>(current_); rate >= 0 && at < kTableSize; --rate)
    for (uint32_t twice = 0; twice < 2 && at < kTableSize; ++twice) indices[at++] = static_cast<uint8_t>(rate);
  while (at < kTableSize) indices[at++] = 0;
}

void WifiRateScaler::on_result(bool success, uint32_t attempts, uint32_t rate_index, uint64_t now_ms) {
  if (attempts == 0) attempts = 1;
  if (attempts > kTableSize) attempts = kTableSize;
  if (rate_index != current_ && rate_index < kRateCount) {
    // a frame that started at another rate than the current one (sent before the table changed): it says nothing about the current rate
    return;
  }
  if (!window_started_) {
    window_started_ = true;
    window_start_ms_ = now_ms;
  }
  ++window_frames_;
  window_attempts_ += attempts;
  if (success) ++window_ok_;
  if (window_frames_ >= kWindowFrames) evaluate(now_ms);
}

void WifiRateScaler::tick(uint64_t now_ms) {
  if (window_started_ && window_frames_ >= kMinFlushFrames && now_ms - window_start_ms_ >= kFlushMs) evaluate(now_ms);
}

void WifiRateScaler::evaluate(uint64_t) {
  const uint32_t c = current_;
  const uint32_t p = window_attempts_ ? window_ok_ * kOne / window_attempts_ : kOne;
  window_frames_ = window_ok_ = window_attempts_ = 0;
  window_started_ = false;
  // what is known about this rate: the new sample counts as much as the old
  chance_[c] = known_[c] ? (chance_[c] + p) / 2 : p;
  known_[c] = true;
  for (uint32_t i = 0; i < kRateCount; ++i) {
    if (i == c) { age_[i] = 0; continue; }
    if (known_[i] && ++age_[i] > kForgetAfterWindows) known_[i] = false;
  }
  for (uint32_t i = 0; i < kRateCount; ++i)
    if (i != c && hold_[i] > 0 && !probing_) --hold_[i];
  if (probing_) {
    probing_ = false;
    if (p < kProbeKeeps && c > 0) {                                // the probe failed: back, and leave that rate alone for a while
      current_ = probe_from_;
      backoff_[c] = backoff_[c] * 2 > kMaxBackoff ? kMaxBackoff : backoff_[c] * 2;
      hold_[c] = backoff_[c];
      changed_ = true;
    } else {
      backoff_[c] = 1;
    }
    return;
  }
  const uint64_t goodput = uint64_t{p} * kRatesKbps[c];
  if (c > 0 && (p < kPoor || (known_[c - 1] && uint64_t{chance_[c - 1]} * kRatesKbps[c - 1] > goodput))) {
    current_ = c - 1;                                              // down
    hold_[c] = backoff_[c];
    changed_ = true;
    return;
  }
  if (c + 1 < kRateCount && p >= kGood && hold_[c + 1] == 0 && (!known_[c + 1] || uint64_t{chance_[c + 1]} * kRatesKbps[c + 1] > goodput)) {
    probing_ = true;                                               // up, as a probe
    probe_from_ = c;
    current_ = c + 1;
    changed_ = true;
  }
}

}  // namespace hardware
