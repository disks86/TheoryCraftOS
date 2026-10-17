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

#include "Gui/Controls/ColorSpectrum.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"

namespace gui {

ColorSpectrum::ColorSpectrum(allocators::IAllocator& allocator) : Control(allocator) {
  field_ = allocate_view<uint32_t>(allocator, uint64_t{kField} * kField);
}

void ColorSpectrum::set_hsv(const Hsv& hsv) {
  UiGuard lock(this);
  hsv_ = hsv;
  hsv_.hue %= 360;
  invalidate();
}

void ColorSpectrum::user_changed(const Hsv& hsv) {
  if (hsv.hue == hsv_.hue && hsv.saturation == hsv_.saturation && hsv.value == hsv_.value) return;
  hsv_ = hsv;
  invalidate();
  fire(changed_);
}

Point ColorSpectrum::field_marker() const {
  return {static_cast<int32_t>(hsv_.saturation) * (kField - 1) / 255, (255 - static_cast<int32_t>(hsv_.value)) * (kField - 1) / 255};
}

int32_t ColorSpectrum::strip_marker() const { return static_cast<int32_t>(hsv_.hue) * (kField - 1) / 359; }

// The square for the current hue: saturation grows to the right, brightness upwards.
void ColorSpectrum::rebuild_field() {
  if (field_.empty()) return;
  Hsv c{hsv_.hue, 0, 0};
  for (int32_t y = 0; y < kField; ++y) {
    c.value = static_cast<uint8_t>(255 - y * 255 / (kField - 1));
    for (int32_t x = 0; x < kField; ++x) {
      c.saturation = static_cast<uint8_t>(x * 255 / (kField - 1));
      field_[uint64_t{static_cast<uint32_t>(y)} * kField + static_cast<uint32_t>(x)] = hsv_to_color(c);
    }
  }
  field_hue_ = hsv_.hue;
}

void ColorSpectrum::paint(Painter& painter) {
  const Theme& t = painter.theme();
  painter.fill({0, 0, bounds_.width, bounds_.height}, t.face);
  if (field_hue_ != hsv_.hue) rebuild_field();
  if (!field_.empty()) {
    graphics::Surface s;
    s.pixels = field_;
    s.width = s.height = s.pitch = kField;
    painter.image(s, {0, 0, static_cast<uint32_t>(kField), static_cast<uint32_t>(kField)}, 0, 0);
  }
  const Rect2D strip = strip_rect();
  for (int32_t y = 0; y < kField; ++y)
    painter.fill({strip.x, y, strip.width, 1}, hsv_to_color(Hsv{static_cast<uint16_t>(y * 359 / (kField - 1)), 255, 255}));
  painter.outline(field_rect(), t.shadow);
  painter.outline(strip, t.shadow);
  // The markers: a ring on the square, a pointer on each side of the strip.
  const Point m = field_marker();
  const uint32_t ring = hsv_.value > 140 && hsv_.saturation < 120 ? 0xFF000000 : 0xFFFFFFFF;
  painter.outline({m.x - 4, m.y - 4, 9, 9}, ring);
  const int32_t my = strip_marker();
  painter.outline({strip.x - 1, my - 2, strip.width + 2, 5}, 0xFF000000);
  painter.outline({strip.x, my - 1, strip.width, 3}, 0xFFFFFFFF);
  if (focused()) painter.focus_frame({-1, -1, bounds_.width + 2, bounds_.height + 2});
}

void ColorSpectrum::pick(const MouseArgs& args) {
  Hsv next = hsv_;
  if (drag_ == Drag::Field) {
    int32_t x = args.x, y = args.y;
    x = x < 0 ? 0 : x > kField - 1 ? kField - 1 : x;
    y = y < 0 ? 0 : y > kField - 1 ? kField - 1 : y;
    next.saturation = static_cast<uint8_t>(x * 255 / (kField - 1));
    next.value = static_cast<uint8_t>(255 - y * 255 / (kField - 1));
  } else if (drag_ == Drag::Strip) {
    const int32_t y = args.y < 0 ? 0 : args.y > kField - 1 ? kField - 1 : args.y;
    next.hue = static_cast<uint16_t>(y * 359 / (kField - 1));
  }
  user_changed(next);
}

void ColorSpectrum::on_mouse_down(const MouseArgs& args) {
  if (args.button != MouseButton::Left) return;
  if (contains(field_rect(), args.x, args.y)) drag_ = Drag::Field;
  else if (contains({strip_rect().x - kGap / 2, 0, strip_rect().width + kGap / 2, static_cast<uint32_t>(kField)}, args.x, args.y)) drag_ = Drag::Strip;
  else return;
  pick(args);
}

void ColorSpectrum::on_mouse_move(const MouseArgs& args) {
  if (drag_ != Drag::None) pick(args);
}

bool ColorSpectrum::on_key(const KeyArgs& args) {
  using namespace hardware;
  Hsv next = hsv_;
  auto clamp = [](int32_t v) { return static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v); };
  switch (args.usage) {
    case key::kLeft: next.saturation = clamp(next.saturation - 8); break;
    case key::kRight: next.saturation = clamp(next.saturation + 8); break;
    case key::kUp: next.value = clamp(next.value + 8); break;
    case key::kDown: next.value = clamp(next.value - 8); break;
    case 0x4B: next.hue = static_cast<uint16_t>((next.hue + 10) % 360); break;       // Page Up
    case 0x4E: next.hue = static_cast<uint16_t>((next.hue + 350) % 360); break;      // Page Down
    default: return false;
  }
  user_changed(next);
  return true;
}

}  // namespace gui
