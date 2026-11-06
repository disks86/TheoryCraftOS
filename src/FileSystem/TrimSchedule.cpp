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

#include "FileSystem/TrimSchedule.hpp"

namespace filesystem {

void TrimSchedule::load(StringView text) {
  last_unix_ = 0;
  const StringView key("last ");
  if (text.size() <= key.size() || !text.substr(0, key.size()).equals(key)) return;
  uint64_t value = 0;
  uint64_t i = key.size();
  for (; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i) {
    const uint64_t digit = static_cast<uint64_t>(text[i] - '0');
    if (value > (UINT64_MAX - digit) / 10) return;   // (overflow: damaged)
    value = value * 10 + digit;
  }
  if (i == key.size() || (i < text.size() && text[i] != '\n')) return;
  last_unix_ = value;
}

bool TrimSchedule::save(String* out) const {
  return out->assign("last ") && out->append_uint(last_unix_) && out->append("\n");
}

bool TrimSchedule::due(uint64_t unix_now, uint64_t uptime_seconds) const {
  if (uptime_seconds < kFirstDelaySeconds) return false;
  // No clock, a clock behind the last run, or no run remembered: once this start.
  if (unix_now == 0 || unix_now < last_unix_ || last_unix_ == 0) return !ran_this_start_;
  return unix_now - last_unix_ >= kIntervalSeconds;
}

void TrimSchedule::done(uint64_t unix_now) {
  ran_this_start_ = true;
  if (unix_now != 0) last_unix_ = unix_now;
}

}  // namespace filesystem
