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

#include "Gui/Controls/ColorPicker.hpp"
#include "Gui/Window.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"

namespace gui {

namespace {
int32_t clamp(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }
}  // namespace

// --- ColorPickerPopup ----------------------------------------------------------

void ColorPickerPopup::paint(Painter& painter) {
  const Theme& t = painter.theme();
  const Hsv& hsv = owner_.hsv();
  const Rect2D all{0, 0, bounds_.width, bounds_.height};
  painter.fill(all, t.face);
  painter.bevel(all, false);

  // Saturation across, brightness down, at the current hue.
  for (int32_t cy = 0; cy < kCells; ++cy)
    for (int32_t cx = 0; cx < kCells; ++cx) {
      const Hsv cell{hsv.hue, static_cast<uint8_t>(cx * 255 / (kCells - 1)), static_cast<uint8_t>(255 - cy * 255 / (kCells - 1))};
      painter.fill({kPad + cx * kCell, kPad + cy * kCell, static_cast<uint32_t>(kCell), static_cast<uint32_t>(kCell)}, hsv_to_color(cell));
    }
  painter.outline({kPad - 1, kPad - 1, static_cast<uint32_t>(kField + 2), static_cast<uint32_t>(kField + 2)}, t.shadow);
  // The hues, top to bottom.
  for (int32_t cy = 0; cy < kCells; ++cy) {
    const Hsv cell{static_cast<uint16_t>((2 * cy + 1) * 360 / (2 * kCells)), 255, 255};
    painter.fill({kStripX, kPad + cy * kCell, static_cast<uint32_t>(kStrip), static_cast<uint32_t>(kCell)}, hsv_to_color(cell));
  }
  painter.outline({kStripX - 1, kPad - 1, static_cast<uint32_t>(kStrip + 2), static_cast<uint32_t>(kField + 2)}, t.shadow);

  // Where the colour is: a ring in the square, a bracket at the hue.
  const int32_t mx = kPad + hsv.saturation * (kField - 1) / 255, my = kPad + (255 - hsv.value) * (kField - 1) / 255;
  painter.outline({mx - 4, my - 4, 9, 9}, 0xFFFFFFFF);
  painter.outline({mx - 3, my - 3, 7, 7}, 0xFF000000);
  const int32_t hy = kPad + hsv.hue * (kField - 1) / 359;
  painter.outline({kStripX - 3, hy - 2, static_cast<uint32_t>(kStrip + 6), 5}, 0xFFFFFFFF);
  painter.outline({kStripX - 2, hy - 1, static_cast<uint32_t>(kStrip + 4), 3}, 0xFF000000);

  // The preview and the code.
  const Rect2D preview{kPad, kPad + kField + kPad, static_cast<uint32_t>(kWidth - 2 * kPad), static_cast<uint32_t>(kPreview)};
  painter.fill(preview, owner_.color());
  painter.outline(preview, t.shadow);
  char code[8];
  format_color(owner_.color(), code);
  painter.text_centered(preview, StringView(code, 7), contrasting_text_color(owner_.color()));
}

void ColorPickerPopup::pick(const MouseArgs& args) {
  Hsv hsv = owner_.hsv();
  if (drag_ == Drag::Field) {
    hsv.saturation = static_cast<uint8_t>(clamp(args.x - kPad, 0, kField - 1) * 255 / (kField - 1));
    hsv.value = static_cast<uint8_t>(255 - clamp(args.y - kPad, 0, kField - 1) * 255 / (kField - 1));
  } else if (drag_ == Drag::Strip) {
    hsv.hue = static_cast<uint16_t>(clamp(args.y - kPad, 0, kField - 1) * 359 / (kField - 1));
  } else {
    return;
  }
  owner_.set_hsv(hsv);
}

void ColorPickerPopup::on_mouse_down(const MouseArgs& args) {
  if (args.button != MouseButton::Left) return;
  if (contains({kPad, kPad, static_cast<uint32_t>(kField), static_cast<uint32_t>(kField)}, args.x, args.y)) drag_ = Drag::Field;
  else if (contains({kStripX, kPad, static_cast<uint32_t>(kStrip), static_cast<uint32_t>(kField)}, args.x, args.y)) drag_ = Drag::Strip;
  else drag_ = Drag::None;
  pick(args);
}

void ColorPickerPopup::on_mouse_move(const MouseArgs& args) {
  if (drag_ != Drag::None) pick(args);  // the window keeps sending moves while the button is held
}

bool ColorPickerPopup::on_key(const KeyArgs& args) {
  using namespace hardware;
  Hsv hsv = owner_.hsv();
  switch (args.usage) {
    case key::kLeft: hsv.saturation = static_cast<uint8_t>(clamp(hsv.saturation - 8, 0, 255)); break;
    case key::kRight: hsv.saturation = static_cast<uint8_t>(clamp(hsv.saturation + 8, 0, 255)); break;
    case key::kUp: hsv.value = static_cast<uint8_t>(clamp(hsv.value + 8, 0, 255)); break;
    case key::kDown: hsv.value = static_cast<uint8_t>(clamp(hsv.value - 8, 0, 255)); break;
    case 0x4B: hsv.hue = static_cast<uint16_t>((hsv.hue + 15) % 360); break;        // Page Up
    case 0x4E: hsv.hue = static_cast<uint16_t>((hsv.hue + 345) % 360); break;       // Page Down
    case key::kEnter:
    case key::kSpace:
    case key::kEscape:
      owner_.close_popup();
      return true;
    default:
      return true;  // a popup swallows other keys
  }
  owner_.set_hsv(hsv);
  return true;
}

// --- ColorPicker ---------------------------------------------------------------

void ColorPicker::set_hsv(const Hsv& hsv) {
  UiGuard lock(this);
  Hsv fixed = hsv;
  fixed.hue %= 360;
  hsv_ = fixed;
  invalidate();
  const uint32_t color = hsv_to_color(fixed);
  if (color == color_) return;
  color_ = color;
  fire(changed_);
}

void ColorPicker::set_color(uint32_t argb) {
  UiGuard lock(this);
  Hsv hsv = color_to_hsv(argb);
  if (hsv.saturation == 0) hsv.hue = hsv_.hue;  // a grey has no hue of its own: keep the old one
  if (hsv.value == 0) hsv.saturation = hsv_.saturation;
  const uint32_t opaque = make_color(red_of(argb), green_of(argb), blue_of(argb));
  hsv_ = hsv;
  invalidate();
  if (opaque == color_) return;
  color_ = opaque;
  fire(changed_);
}

void ColorPicker::paint(Painter& painter) {
  const Theme& t = painter.theme();
  const Rect2D all{0, 0, bounds_.width, bounds_.height};
  painter.fill(all, t.field);
  painter.bevel(all, true);
  const int32_t w = static_cast<int32_t>(bounds_.width), h = static_cast<int32_t>(bounds_.height);
  const uint32_t text_color = enabled() ? t.text : t.disabled_text;
  const Rect2D swatch{4, 4, 24, static_cast<uint32_t>(h > 8 ? h - 8 : 0)};
  painter.fill(swatch, enabled() ? color_ : t.face);
  painter.outline(swatch, t.shadow);
  char code[8];
  format_color(color_, code);
  const Rect2D label{32, 3, static_cast<uint32_t>(w > 56 ? w - 56 : 0), static_cast<uint32_t>(h > 6 ? h - 6 : 0)};
  if (focused() && !open_) painter.fill(label, t.selection);
  painter.text(label.x + 1, (h - Painter::kGlyphHeight) / 2, StringView(code, 7), focused() && !open_ ? t.selection_text : text_color);
  const Rect2D button{w - 19, 2, 17, static_cast<uint32_t>(h > 4 ? h - 4 : 0)};
  painter.fill(button, t.face);
  painter.bevel(button, open_);
  const int32_t cx = button.x + 8 + (open_ ? 1 : 0), cy = h / 2 - 1 + (open_ ? 1 : 0);
  for (int32_t i = 0; i < 4; ++i) painter.line(cx - 3 + i, cy + i - 1, cx + 3 - i, cy + i - 1, text_color);
}

void ColorPicker::open_popup() {
  Window* w = window();
  if (!w) return;
  const Point o = window_origin();
  if (w->open_popup(&popup_, this, o.x, o.y + static_cast<int32_t>(bounds_.height))) open_ = true;
  invalidate();
}

void ColorPicker::close_popup() {
  UiGuard lock(this);
  if (open_ && window()) window()->close_popup();
  open_ = false;
}

void ColorPicker::on_mouse_down(const MouseArgs& args) {
  if (args.button == MouseButton::Left) open_popup();  // a click while open is consumed by the window
}

bool ColorPicker::on_key(const KeyArgs& args) {
  using namespace hardware;
  if (args.usage == key::kSpace || args.usage == key::kEnter ||
      (args.usage == key::kDown && (args.modifiers & (modifier::kLeftAlt | modifier::kRightAlt)))) {
    open_popup();
    return true;
  }
  return false;
}

}  // namespace gui
