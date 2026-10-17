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

// A palette of color swatches with the two colors in use: the primary (foreground) and the secondary (background),
// shown as two overlapping squares at the left. A left click on a swatch makes it the primary color, a right click the
// secondary; clicking the squares swaps them. The swatches are laid out row by row in `rows` rows (default 2). The program can
// set the colors too (a color picker's choice, the eyedropper). Colors are opaque 0xFFRRGGBB.
#pragma once
#include "Gui/Control.hpp"
#include "ObjectContainer/List.hpp"

namespace gui {

class ColorPalette : public Control {
 public:
  static constexpr int32_t kSwatch = 16, kGap = 2, kPad = 4, kIndicator = 40;

  // Starts with the classic 28 colors, black as the primary and white as the secondary.
  explicit ColorPalette(allocators::IAllocator& allocator);

  // Replaces the swatches. False if memory ran out (the old ones stay).
  bool set_colors(const uint32_t* colors, uint32_t count);
  void set_rows(uint32_t rows);
  uint32_t color_count() const { return static_cast<uint32_t>(colors_.count()); }
  uint32_t color_at(uint32_t index) const { return index < colors_.count() ? colors_[index] : 0; }

  uint32_t primary() const { return primary_; }
  uint32_t secondary() const { return secondary_; }
  // The handler fires when either color changes.
  void set_primary(uint32_t argb);
  void set_secondary(uint32_t argb);
  void swap_colors();
  void on_colors_changed(Handler handler) { UiGuard lock(this); changed_ = handler; }

  // Geometry (for painting and tests), in this control's coordinates.
  Rect2D swatch_rect(uint32_t index) const;
  int32_t swatch_at(int32_t x, int32_t y) const;  // -1: none
  Rect2D indicator_rect() const { return {kPad, kPad, static_cast<uint32_t>(kIndicator), static_cast<uint32_t>(kIndicator)}; }
  Rect2D primary_rect() const { return {kPad, kPad, 24, 24}; }
  Rect2D secondary_rect() const { return {kPad + 14, kPad + 14, 24, 24}; }
  uint32_t columns() const { return rows_ ? static_cast<uint32_t>((colors_.count() + rows_ - 1) / rows_) : 0; }

  void paint(Painter& painter) override;
  void on_mouse_down(const MouseArgs& args) override;

 protected:
  Size default_size() const override {
    return {2 * kPad + kIndicator + kPad + static_cast<int32_t>(columns()) * (kSwatch + kGap),
            2 * kPad + static_cast<int32_t>(rows_) * (kSwatch + kGap)};
  }

 private:
  List<uint32_t> colors_;
  uint32_t rows_ = 2;
  uint32_t primary_ = 0xFF000000, secondary_ = 0xFFFFFFFF;
  Handler changed_;
};

}  // namespace gui
