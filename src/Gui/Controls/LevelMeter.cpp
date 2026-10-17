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

#include "Gui/Controls/LevelMeter.hpp"

namespace gui {

namespace {
constexpr uint32_t kGreen = 0xFF00C000, kYellow = 0xFFE0E000, kRed = 0xFFE00000;
constexpr uint32_t kGreenDim = 0xFF004000, kYellowDim = 0xFF404000, kRedDim = 0xFF400000;
}  // namespace

void LevelMeter::set_range(int32_t minimum, int32_t maximum) {
  UiGuard lock(this);
  min_ = minimum;
  max_ = maximum < minimum ? minimum : maximum;
  level_ = level_ < min_ ? min_ : level_ > max_ ? max_ : level_;
  peak_ = peak_ < min_ ? min_ : peak_ > max_ ? max_ : peak_;
  invalidate();
}

void LevelMeter::set_level(int32_t level) {
  UiGuard lock(this);
  if (level < min_) level = min_;
  if (level > max_) level = max_;
  const int32_t old_peak = peak_;
  if (level > peak_ || !peak_hold_) peak_ = level;
  if (level == level_ && peak_ == old_peak) return;
  level_ = level;
  invalidate();
}

int32_t LevelMeter::segment_count() const {
  const int32_t inner = static_cast<int32_t>(orientation_ == Orientation::Horizontal ? bounds_.width : bounds_.height) - 4;
  return inner > 0 ? (inner + kGap) / (kSegment + kGap) : 0;
}

// Segments lit for a level: the share of the range, rounded up so any signal shows at all.
int32_t LevelMeter::lit_segments(int32_t level) const {
  const int64_t span = int64_t{max_} - min_;
  const int32_t n = segment_count();
  if (span <= 0 || n == 0 || level <= min_) return 0;
  return static_cast<int32_t>((int64_t{level - min_} * n + span - 1) / span);
}

void LevelMeter::paint(Painter& painter) {
  const Theme& t = painter.theme();
  const Rect2D all{0, 0, bounds_.width, bounds_.height};
  painter.fill(all, 0xFF101010);
  painter.bevel(all, true);
  const int32_t n = segment_count();
  const int32_t lit = lit_segments(level_);
  const int32_t peak = peak_hold_ ? lit_segments(peak_) : 0;
  const bool horizontal = orientation_ == Orientation::Horizontal;
  const int32_t thickness = static_cast<int32_t>(horizontal ? bounds_.height : bounds_.width) - 6;
  for (int32_t i = 0; i < n; ++i) {
    const int32_t share = (i + 1) * 100 / n;  // how far up the range this segment ends
    const bool is_lit = i < lit || (peak > 0 && i == peak - 1);
    uint32_t color;
    if (share <= 70) color = is_lit ? kGreen : kGreenDim;
    else if (share <= 90) color = is_lit ? kYellow : kYellowDim;
    else color = is_lit ? kRed : kRedDim;
    if (!enabled()) color = is_lit ? t.shadow : t.dark;
    const int32_t start = 2 + i * (kSegment + kGap);
    if (horizontal) painter.fill({start, 3, static_cast<uint32_t>(kSegment), static_cast<uint32_t>(thickness)}, color);
    else painter.fill({3, static_cast<int32_t>(bounds_.height) - 2 - start - kSegment, static_cast<uint32_t>(thickness), static_cast<uint32_t>(kSegment)}, color);
  }
}

}  // namespace gui
