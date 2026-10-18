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

#include "Desktop/ColorDialog.hpp"
#include "Gui/Window.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"

namespace desktop {

using namespace gui;

void ColorDialog::Swatch::paint(Painter& painter) {
  const Rect2D all{0, 0, bounds_.width, bounds_.height};
  painter.fill(all, color);
  painter.bevel(all, true);
}

ColorDialog::ColorDialog(allocators::IAllocator& allocator)
    : Frame(allocator),
      top_(allocator),
      form_(allocator),
      caption_row_(allocator),
      preview_row_(allocator),
      palette_row_(allocator),
      button_row_(allocator),
      gap_(allocator),
      spectrum_(allocator),
      new_label_(allocator),
      old_label_(allocator),
      new_swatch_(allocator),
      old_swatch_(allocator),
      red_label_(allocator),
      green_label_(allocator),
      blue_label_(allocator),
      hex_label_(allocator),
      red_row_(allocator),
      green_row_(allocator),
      blue_row_(allocator),
      hex_row_(allocator),
      red_(allocator),
      green_(allocator),
      blue_(allocator),
      hex_(allocator),
      basic_(allocator),
      ok_(allocator),
      cancel_(allocator) {
  set_title("Color");
  set_minimize_enabled(false);
  set_maximize_enabled(false);
  set_resizable(false);
  set_escape_closes(true);
  set_padding(Insets::all(8));

  // The bottom, from the lowest row up: the buttons, the basic colors.
  button_row_.set_style(PanelStyle::Transparent);
  button_row_.set_dock(DockStyle::Bottom);
  button_row_.set_size(0, 32);
  button_row_.set_padding(Insets{0, 4, 0, 0});
  cancel_.set_text("Cancel");
  cancel_.set_dock(DockStyle::Right);
  cancel_.set_size(92, 0);
  cancel_.on_click({&ColorDialog::on_cancel, this});
  gap_.set_style(PanelStyle::Transparent);
  gap_.set_dock(DockStyle::Right);
  gap_.set_size(8, 0);
  ok_.set_text("OK");
  ok_.set_dock(DockStyle::Right);
  ok_.set_size(92, 0);
  ok_.on_click({&ColorDialog::on_ok, this});
  button_row_.add(&cancel_);
  button_row_.add(&gap_);
  button_row_.add(&ok_);

  palette_row_.set_style(PanelStyle::Transparent);
  palette_row_.set_dock(DockStyle::Bottom);
  palette_row_.set_size(0, 52);
  palette_row_.set_padding(Insets{0, 4, 0, 0});
  basic_.set_indicator_visible(false);
  basic_.set_dock(DockStyle::Left);
  basic_.set_size(basic_.preferred_size().width, 0);
  basic_.on_colors_changed({&ColorDialog::on_basic, this});
  palette_row_.add(&basic_);

  // Above them: the spectrum at the left and the form at the right.
  top_.set_style(PanelStyle::Transparent);
  top_.set_dock(DockStyle::Fill);
  spectrum_.set_dock(DockStyle::Left);
  spectrum_.set_size(ColorSpectrum::kField + ColorSpectrum::kGap + ColorSpectrum::kStrip + 12, 0);
  spectrum_.on_color_changed({&ColorDialog::on_spectrum, this});
  form_.set_style(PanelStyle::Transparent);
  form_.set_dock(DockStyle::Fill);
  top_.add(&spectrum_);
  top_.add(&form_);

  // Under the captions "New" and "Current": two squares side by side.
  caption_row_.set_style(PanelStyle::Transparent);
  caption_row_.set_dock(DockStyle::Top);
  caption_row_.set_size(0, 22);
  new_label_.set_text("New");
  new_label_.set_dock(DockStyle::Left);
  new_label_.set_size(78, 0);
  old_label_.set_text("Current");
  old_label_.set_dock(DockStyle::Left);
  old_label_.set_size(78, 0);
  caption_row_.add(&new_label_);
  caption_row_.add(&old_label_);
  preview_row_.set_style(PanelStyle::Transparent);
  preview_row_.set_dock(DockStyle::Top);
  preview_row_.set_size(0, 44);
  preview_row_.set_padding(Insets{0, 0, 0, 6});
  new_swatch_.set_dock(DockStyle::Left);
  new_swatch_.set_size(74, 0);
  old_swatch_.set_dock(DockStyle::Left);
  old_swatch_.set_size(74, 0);
  preview_row_.add(&new_swatch_);
  preview_row_.add(&old_swatch_);
  form_.add(&caption_row_);
  form_.add(&preview_row_);

  struct Component {
    Panel& row;
    Label& label;
    const char* text;
  };
  const Component rows[4] = {{red_row_, red_label_, "Red"}, {green_row_, green_label_, "Green"}, {blue_row_, blue_label_, "Blue"}, {hex_row_, hex_label_, "Hex"}};
  for (const Component& r : rows) {
    r.row.set_style(PanelStyle::Transparent);
    r.row.set_dock(DockStyle::Top);
    r.row.set_size(0, 30);
    r.row.set_padding(Insets{0, 3, 0, 3});
    r.label.set_text(r.text);
    r.label.set_dock(DockStyle::Left);
    r.label.set_size(56, 0);
    r.row.add(&r.label);
    form_.add(&r.row);
  }
  SpinBox* boxes[3] = {&red_, &green_, &blue_};
  Panel* parents[3] = {&red_row_, &green_row_, &blue_row_};
  for (int i = 0; i < 3; ++i) {
    boxes[i]->set_range(0, 255);
    boxes[i]->set_dock(DockStyle::Left);
    boxes[i]->set_size(84, 0);
    boxes[i]->on_value_changed({&ColorDialog::on_component, this});
    parents[i]->add(boxes[i]);
  }
  hex_.set_max_length(7);
  hex_.set_dock(DockStyle::Left);
  hex_.set_size(84, 0);
  hex_.on_changed({&ColorDialog::on_hex, this});
  hex_row_.add(&hex_);

  add(&button_row_);
  add(&palette_row_);
  add(&top_);  // Fill goes last
  apply(0xFF000000, true, true, true);
}

bool ColorDialog::show(WindowManager& manager, StringView title, uint32_t initial, Handler result) {
  UiGuard lock(&manager);
  if (showing()) return false;
  set_title(title);
  accepted_ = false;
  old_swatch_.color = initial | 0xFF000000u;
  set_color(initial);
  on_closed(result);
  const int32_t area_w = static_cast<int32_t>(manager.dialog_area().width), area_h = static_cast<int32_t>(manager.dialog_area().height);
  const int32_t w = kWidth < area_w ? kWidth : area_w, h = kHeight < area_h ? kHeight : area_h;
  set_bounds({manager.dialog_area().x + ((area_w - w) / 2 > 0 ? (area_w - w) / 2 : 0), manager.dialog_area().y + ((area_h - h) / 3 > 0 ? (area_h - h) / 3 : 0), static_cast<uint32_t>(w), static_cast<uint32_t>(h)});
  if (!manager.add_window(this, /*modal=*/true)) return false;
  if (Window* win = window()) {
    win->layout();
    win->set_focus(&ok_);
  }
  return true;
}

// Makes `argb` the color and refreshes the parts that did not cause the change.
void ColorDialog::apply(uint32_t argb, bool update_spectrum, bool update_components, bool update_hex) {
  if (updating_) return;
  updating_ = true;
  color_ = argb | 0xFF000000u;
  new_swatch_.color = color_;
  new_swatch_.invalidate();
  if (update_spectrum) {
    Hsv hsv = color_to_hsv(color_);
    if (hsv.saturation == 0 || hsv.value == 0) hsv.hue = spectrum_.hsv().hue;   // a grey keeps the hue it was chosen with
    spectrum_.set_hsv(hsv);
  }
  if (update_components) {
    red_.set_value(red_of(color_));
    green_.set_value(green_of(color_));
    blue_.set_value(blue_of(color_));
  }
  if (update_hex) {
    static const char kDigits[] = "0123456789ABCDEF";
    char text[8] = {'#', kDigits[(color_ >> 20) & 15], kDigits[(color_ >> 16) & 15], kDigits[(color_ >> 12) & 15], kDigits[(color_ >> 8) & 15],
                    kDigits[(color_ >> 4) & 15], kDigits[color_ & 15], 0};
    hex_.set_text(StringView(text, 7));
  }
  updating_ = false;
}

void ColorDialog::set_color(uint32_t argb) { apply(argb, true, true, true); }

void ColorDialog::on_spectrum(void* self, Control&) {
  auto* d = static_cast<ColorDialog*>(self);
  d->apply(d->spectrum_.color(), false, true, true);
}

void ColorDialog::on_component(void* self, Control&) {
  auto* d = static_cast<ColorDialog*>(self);
  if (d->updating_) return;
  d->apply(make_color(static_cast<uint8_t>(d->red_.value()), static_cast<uint8_t>(d->green_.value()), static_cast<uint8_t>(d->blue_.value())), true, false, true);
}

// A complete #RRGGBB (or RRGGBB) typed in the box is taken at once; anything else waits.
void ColorDialog::on_hex(void* self, Control&) {
  auto* d = static_cast<ColorDialog*>(self);
  if (d->updating_) return;
  StringView text = d->hex_.text();
  if (!text.empty() && text[0] == '#') text = text.without_prefix(1);
  if (text.size() != 6) return;
  uint32_t value = 0;
  for (uint64_t i = 0; i < 6; ++i) {
    const char c = text[i];
    const uint32_t digit = c >= '0' && c <= '9' ? static_cast<uint32_t>(c - '0') : c >= 'a' && c <= 'f' ? static_cast<uint32_t>(c - 'a' + 10) : c >= 'A' && c <= 'F' ? static_cast<uint32_t>(c - 'A' + 10) : 16;
    if (digit > 15) return;
    value = value * 16 + digit;
  }
  d->apply(value, true, true, false);
}

void ColorDialog::on_basic(void* self, Control&) {
  auto* d = static_cast<ColorDialog*>(self);
  d->set_color(d->basic_.primary());
}

void ColorDialog::on_ok(void* self, Control&) {
  auto* d = static_cast<ColorDialog*>(self);
  d->accepted_ = true;
  d->close();
}

void ColorDialog::on_cancel(void* self, Control&) { static_cast<ColorDialog*>(self)->close(); }

bool ColorDialog::on_key(const KeyArgs& args) {
  if (args.usage == hardware::key::kEnter) {
    ok_.click();
    return true;
  }
  return Frame::on_key(args);
}

}  // namespace desktop
