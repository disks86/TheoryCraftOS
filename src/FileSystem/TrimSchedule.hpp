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

// When the system trims its volumes by itself (IFileSystem::trim, what Linux's fstrim.timer does weekly): kFirstDelaySeconds after start-up
// (so the boot is not slowed), then every kIntervalSeconds of wall-clock time. The last run is remembered in a small text file
// (layout::kFstrimState) so that a machine started every day still trims about once a week. Without a working clock (unix time 0, or a
// clock that went backwards) it trims once per start instead. Pure policy: the caller reads and writes the file and does the trimming.
#pragma once
#include <stdint.h>
#include "Utility/String.hpp"
#include "Utility/StringView.hpp"

namespace filesystem {

class TrimSchedule {
 public:
  static constexpr uint64_t kFirstDelaySeconds = 600;
  static constexpr uint64_t kIntervalSeconds = 7 * 24 * 3600;

  // The state file's contents ("last <unix seconds>\n"); anything else (no file, damage) counts as never.
  void load(StringView text);
  bool save(String* out) const;

  bool due(uint64_t unix_now, uint64_t uptime_seconds) const;
  // The trim ran (unix_now 0: no clock; this start only).
  void done(uint64_t unix_now);

  uint64_t last_unix() const { return last_unix_; }

 private:
  uint64_t last_unix_ = 0;
  bool ran_this_start_ = false;
};

}  // namespace filesystem
