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
#include "Kernel/Sync.hpp"
#include "Hardware/Mouse/IMouse.hpp"

namespace hardware {

// Presents several mice/tablets as one stream of events. Children are not
// owned. Absolute and relative events can interleave; check MouseEvent::absolute
// (and use the producing device's max_x()/max_y() through at()). Mice may be
// added and removed while other threads poll (USB hot-plug); a removed mouse's
// remaining events are still delivered, and nothing calls into it once remove()
// has returned.
class CompositeMouse final : public IMouse {
 public:
  static constexpr uint32_t kMaxMice = 8;

  constexpr CompositeMouse() = default;

  const char* name() const override { return "mice"; }
  bool initialize() override { return true; }
  void shutdown() override {}

  bool add(IMouse* mouse);
  // Takes a mouse out; its remaining events (a button release, if the caller asked it to) are kept and delivered by poll() first.
  bool remove(IMouse* mouse);
  uint32_t count() const;
  // (A pointer for use on the thread that adds and removes mice, or while none can be removed.)
  IMouse* at(uint32_t index) const;

  bool poll(MouseEvent* out) override;
  bool is_absolute() const override { return false; }
  uint32_t max_x() const override { return 0; }
  uint32_t max_y() const override { return 0; }

 private:
  static constexpr uint32_t kOrphans = 64;

  mutable kernel::Mutex mutex_;  // polling threads take turns, and the list changes under it
  MouseEvent orphans_[kOrphans] = {};   // events of mice that were removed
  uint32_t orphan_head_ = 0, orphan_count_ = 0;
  IMouse* mice_[kMaxMice] = {};
  uint32_t count_ = 0;
  uint32_t next_ = 0;
};

}  // namespace hardware
