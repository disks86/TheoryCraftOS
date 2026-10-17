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
#include "Gui/Color.hpp"
#include "Gui/Control.hpp"

namespace gui {

class ColorPicker;

// The panel that drops down from a ColorPicker (a popup, drawn above the other controls): a square of
// saturation (across) and brightness (down) at the current hue, a strip of hues beside it, and a preview of
// the colour with its hex code. Drag in either area; the colour changes live. Keys: left/right
// saturation, up/down brightness, Page Up/Down hue, Enter / Space / Escape close it.
class ColorPickerPopup final : public Control {
 public:
  static constexpr int32_t kPad = 6, kCells = 16, kCell = 8, kField = kCells * kCell, kStrip = 16, kGap = 8, kPreview = 14;
  static constexpr int32_t kWidth = kPad + kField + kGap + kStrip + kPad;
  static constexpr int32_t kHeight = kPad + kField + kPad + kPreview + kPad;

  ColorPickerPopup(allocators::IAllocator& allocator, ColorPicker& owner) : Control(allocator), owner_(owner) {}
  void paint(Painter& painter) override;
  void on_mouse_down(const MouseArgs& args) override;
  void on_mouse_move(const MouseArgs& args) override;
  void on_mouse_up(const MouseArgs&) override { drag_ = Drag::None; }
  bool on_key(const KeyArgs& args) override;

 protected:
  Size default_size() const override { return {kWidth, kHeight}; }

 private:
  enum class Drag : uint8_t { None, Field, Strip };
  static constexpr int32_t kStripX = kPad + kField + kGap;

  void pick(const MouseArgs& args);

  ColorPicker& owner_;
  Drag drag_ = Drag::None;
};

// Shows a colour (a swatch and its #RRGGBB code); clicking it (or Space, Enter, Alt+Down) opens the
// ColorPickerPopup to change it. Colours are opaque 0xFFRRGGBB; the picker also remembers the colour
// as hue / saturation / brightness, so a grey keeps the hue it was picked with.
class ColorPicker : public Control {
 public:
  explicit ColorPicker(allocators::IAllocator& allocator)
      : Control(allocator), hsv_{0, 255, 255}, color_(hsv_to_color(hsv_)), popup_(allocator, *this) {}

  uint32_t color() const { return color_; }
  const Hsv& hsv() const { return hsv_; }
  // The alpha is ignored. The handler fires only when the colour changes.
  void set_color(uint32_t argb);
  void set_hsv(const Hsv& hsv);
  void on_color_changed(Handler handler) { UiGuard lock(this); changed_ = handler; }
  bool open() const { return open_; }
  void open_popup();
  void close_popup();

  bool focusable() const override { return true; }
  void paint(Painter& painter) override;
  void on_mouse_down(const MouseArgs& args) override;
  bool on_key(const KeyArgs& args) override;
  void on_popup_closed(Control*) override { open_ = false; invalidate(); }

 protected:
  Size default_size() const override { return {112, 24}; }

 private:
  Hsv hsv_;
  uint32_t color_;
  Handler changed_;
  ColorPickerPopup popup_;
  bool open_ = false;
};

}  // namespace gui
