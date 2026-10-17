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
#include "Gui/Control.hpp"

namespace gui {

// A bar made of lit segments that shows how high a level is - an audio signal, a load, a fill. It does
// not take input. Green up to 70% of the range, yellow up to 90%, red above. With peak hold on (the
// default) the highest level seen stays marked by one bright segment until reset_peak().
// Horizontal meters fill to the right, vertical ones upwards.
class LevelMeter : public Control {
 public:
  explicit LevelMeter(allocators::IAllocator& allocator, Orientation orientation = Orientation::Horizontal)
      : Control(allocator), orientation_(orientation) {}

  int32_t level() const { return level_; }
  int32_t peak() const { return peak_; }
  int32_t minimum() const { return min_; }
  int32_t maximum() const { return max_; }
  // The level and peak are clamped into the new range. A maximum below the minimum is raised to it.
  void set_range(int32_t minimum, int32_t maximum);
  // Clamped to the range. Raises the peak if it is higher.
  void set_level(int32_t level);
  void reset_peak() { UiGuard lock(this); peak_ = level_; invalidate(); }
  void set_peak_hold(bool hold) { UiGuard lock(this); peak_hold_ = hold; invalidate(); }
  bool peak_hold() const { return peak_hold_; }
  Orientation orientation() const { return orientation_; }

  void paint(Painter& painter) override;

  static constexpr int32_t kSegment = 6, kGap = 2;  // thickness of a lit block and the dark line between blocks

  // How many segments fit and how many are lit for a level (for painting and tests).
  int32_t segment_count() const;
  int32_t lit_segments(int32_t level) const;

 protected:
  Size default_size() const override {
    return orientation_ == Orientation::Horizontal ? Size{120, 18} : Size{18, 120};
  }

 private:
  Orientation orientation_;
  int32_t min_ = 0, max_ = 100, level_ = 0, peak_ = 0;
  bool peak_hold_ = true;
};

}  // namespace gui
