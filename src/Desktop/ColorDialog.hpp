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

// The color dialog: a modal window in which the user chooses any color. A spectrum (saturation and brightness over one hue, and a
// hue strip), red / green / blue number boxes and a #RRGGBB box all show and change the same color, a row of basic colors
// gives quick choices, and a preview shows the new color beside the one it started from. OK (or Enter) accepts, Cancel, Escape or
// the close button leave accepted() false. One dialog serves every program: show() is given the handler to run once the dialog is gone.
#pragma once
#include "Gui/Controls/Button.hpp"
#include "Gui/Controls/ColorPalette.hpp"
#include "Gui/Controls/ColorSpectrum.hpp"
#include "Gui/Controls/Label.hpp"
#include "Gui/Controls/Panel.hpp"
#include "Gui/Controls/SpinBox.hpp"
#include "Gui/Controls/TextBox.hpp"
#include "Gui/Windows/Frame.hpp"
#include "Gui/Windows/WindowManager.hpp"

namespace desktop {

class ColorDialog : public gui::Frame {
 public:
  static constexpr int32_t kWidth = 420, kHeight = 360;

  explicit ColorDialog(allocators::IAllocator& allocator);

  // Opens the dialog modally in `manager`, showing `initial` (opaque; the alpha is ignored). `result` runs once the dialog has closed,
  // accepted or not. False if it is already showing.
  bool show(gui::WindowManager& manager, StringView title, uint32_t initial, gui::Handler result);
  bool showing() const { return manager() != nullptr; }

  // The color being chosen; after the result handler runs and accepted() is true, the choice.
  uint32_t color() const { return color_; }
  bool accepted() const { return accepted_; }
  // Chooses a color from code, as if the user had.
  void set_color(uint32_t argb);

  gui::ColorSpectrum& spectrum() { return spectrum_; }
  gui::SpinBox& red_box() { return red_; }
  gui::SpinBox& green_box() { return green_; }
  gui::SpinBox& blue_box() { return blue_; }
  gui::TextBox& hex_box() { return hex_; }
  gui::ColorPalette& basic_colors() { return basic_; }
  gui::Button& ok_button() { return ok_; }
  gui::Button& cancel_button() { return cancel_; }
  // The two preview squares' colors: the new one and the one the dialog started from.
  uint32_t preview_color() const { return new_swatch_.color; }
  uint32_t original_color() const { return old_swatch_.color; }

  bool on_key(const gui::KeyArgs& args) override;

 private:
  // A square of one color.
  class Swatch final : public gui::Control {
   public:
    explicit Swatch(allocators::IAllocator& allocator) : Control(allocator) {}
    void paint(gui::Painter& painter) override;
    uint32_t color = 0xFF000000;

   protected:
    gui::Size default_size() const override { return {60, 30}; }
  };

  static void on_spectrum(void* self, gui::Control&);
  static void on_component(void* self, gui::Control&);
  static void on_hex(void* self, gui::Control&);
  static void on_basic(void* self, gui::Control&);
  static void on_ok(void* self, gui::Control&);
  static void on_cancel(void* self, gui::Control&);
  void apply(uint32_t argb, bool update_spectrum, bool update_components, bool update_hex);

  gui::Panel top_, form_, caption_row_, preview_row_, palette_row_, button_row_, gap_;
  gui::ColorSpectrum spectrum_;
  gui::Label new_label_, old_label_;
  Swatch new_swatch_, old_swatch_;
  gui::Label red_label_, green_label_, blue_label_, hex_label_;
  gui::Panel red_row_, green_row_, blue_row_, hex_row_;
  gui::SpinBox red_, green_, blue_;
  gui::TextBox hex_;
  gui::ColorPalette basic_;
  gui::Button ok_, cancel_;
  uint32_t color_ = 0xFF000000;
  bool accepted_ = false, updating_ = false;
};

}  // namespace desktop
