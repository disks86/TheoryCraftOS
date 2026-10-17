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

// A color chooser surface: a square showing every saturation (across) and brightness (down) of one hue, and a strip beside it
// to pick the hue. Click or drag in either; the marker shows the color. Keys: arrows change saturation and brightness, Page Up /
// Page Down the hue. The color is kept as hue / saturation / brightness, so a grey keeps the hue it was chosen with (see Gui/Color.hpp).
#pragma once
#include "Gui/Color.hpp"
#include "Gui/Control.hpp"
#include "Utility/AllocatedView.hpp"

namespace gui {

class ColorSpectrum : public Control {
 public:
  static constexpr int32_t kField = 160, kStrip = 18, kGap = 8, kBorder = 1;

  explicit ColorSpectrum(allocators::IAllocator& allocator);
  ~ColorSpectrum() { release_view(allocator(), field_); }

  const Hsv& hsv() const { return hsv_; }
  uint32_t color() const { return hsv_to_color(hsv_); }
  // The handler fires when the color changes by the user (not by these setters).
  void set_hsv(const Hsv& hsv);
  void set_color(uint32_t argb) { set_hsv(color_to_hsv(argb)); }
  void on_color_changed(Handler handler) { UiGuard lock(this); changed_ = handler; }

  // Geometry (for painting and tests), in this control's coordinates.
  Rect2D field_rect() const { return {0, 0, static_cast<uint32_t>(kField), static_cast<uint32_t>(kField)}; }
  Rect2D strip_rect() const { return {kField + kGap, 0, static_cast<uint32_t>(kStrip), static_cast<uint32_t>(kField)}; }
  // Where the markers are.
  Point field_marker() const;
  int32_t strip_marker() const;

  bool focusable() const override { return true; }
  void paint(Painter& painter) override;
  void on_mouse_down(const MouseArgs& args) override;
  void on_mouse_move(const MouseArgs& args) override;
  void on_mouse_up(const MouseArgs&) override { drag_ = Drag::None; }
  bool on_key(const KeyArgs& args) override;

 protected:
  Size default_size() const override { return {kField + kGap + kStrip, kField}; }

 private:
  enum class Drag : uint8_t { None, Field, Strip };

  void pick(const MouseArgs& args);
  void user_changed(const Hsv& hsv);
  void rebuild_field();

  Hsv hsv_{0, 255, 255};
  ArrayView<uint32_t> field_;  // the square drawn for field_hue_
  int32_t field_hue_ = -1;
  Drag drag_ = Drag::None;
  Handler changed_;
};

}  // namespace gui
