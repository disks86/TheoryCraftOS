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
// not take input. By default green up to 70% of the range, yellow up to 90%, red above (a load: high is bad).
// set_zones() changes where the colours change and which way they run, so the same meter can show a fill that
// is good when full (a battery: red when low, green when full) or a temperature with its own limits.
// With peak hold on (the default) the highest level seen stays marked by one bright segment until reset_peak().
// Horizontal meters fill to the right, vertical ones upwards.
enum class MeterZone : uint8_t { Green, Yellow, Red };
// Which end of the range is the bad one.
enum class ZoneDirection : uint8_t {
  HighIsBad,   // green up to the first break, yellow up to the second, red above
  LowIsBad     // red up to the first break, yellow up to the second, green above
};

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
  // The two breaks are levels in the meter's range (first <= second; swapped if not): a level at a break belongs to the zone below it. A change of range
  // does not move them (set the zones after the range).
  void set_zones(ZoneDirection direction, int32_t first_break, int32_t second_break);
  // Back to green up to 70% of the range, yellow up to 90%, red above, following the range.
  void use_default_zones() { UiGuard lock(this); custom_zones_ = false; direction_ = ZoneDirection::HighIsBad; invalidate(); }
  ZoneDirection zone_direction() const { return direction_; }
  // The zone a level falls in.
  MeterZone zone_at(int32_t level) const;
  void reset_peak() { UiGuard lock(this); peak_ = level_; invalidate(); }
  void set_peak_hold(bool hold) { UiGuard lock(this); peak_hold_ = hold; invalidate(); }
  bool peak_hold() const { return peak_hold_; }
  Orientation orientation() const { return orientation_; }

  void paint(Painter& painter) override;
  bool wheel_to_parent() const override { return true; }

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
  bool custom_zones_ = false;
  ZoneDirection direction_ = ZoneDirection::HighIsBad;
  int32_t first_break_ = 0, second_break_ = 0;
};

}  // namespace gui
