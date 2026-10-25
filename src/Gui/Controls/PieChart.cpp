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

#include "Gui/Controls/PieChart.hpp"
#include "Gui/Color.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"
#include "Utility/String.hpp"

namespace gui {

namespace {

constexpr uint32_t kTurn = 65536;  // a full circle in angle units

constexpr uint32_t kPalette[] = {0xFF4E79A7, 0xFFF28E2B, 0xFFE15759, 0xFF76B7B2, 0xFF59A14F, 0xFFEDC948, 0xFFB07AA1, 0xFF9C755F};
constexpr uint32_t kPaletteSize = sizeof(kPalette) / sizeof(kPalette[0]);

// The angle of the first-octant ratio small / large as a share of a turn (0..8192): the straight line plus a parabola, within about 0.2 degrees of
// the true arctangent.
uint32_t octant_angle(uint32_t small, uint32_t large) {
  if (large == 0) return 0;
  const uint64_t t = (uint64_t{small} << 16) / large;  // Q16, 0..65536
  return static_cast<uint32_t>(((t * 8192) >> 16) + ((((t * (65536 - t)) >> 16) * 2848) >> 16));
}

// The angle of the vector (dx, dy) clockwise from straight up (dy grows downwards), 0..65535. The origin has angle 0.
uint32_t angle_of(int32_t dx, int32_t dy) {
  const uint32_t ax = static_cast<uint32_t>(dx < 0 ? -int64_t{dx} : int64_t{dx});
  const uint32_t ay = static_cast<uint32_t>(dy < 0 ? -int64_t{dy} : int64_t{dy});
  // q: the angle from the horizontal axis, 0..16384 (a quarter turn).
  const uint32_t q = ax >= ay ? octant_angle(ay, ax) : 16384 - octant_angle(ax, ay);
  uint32_t a;
  if (dx >= 0) a = dy < 0 ? 16384 - q : 16384 + q;
  else a = dy >= 0 ? 49152 - q : 49152 + q;
  return a % kTurn;
}

// value * scale / sum for value <= sum, without overflow: both are halved until the product fits (the share changes by far less than a pixel).
uint32_t share(uint64_t value, uint64_t sum, uint32_t scale, bool round) {
  while (sum > UINT64_MAX / scale) {
    sum >>= 1;
    value >>= 1;
  }
  if (sum == 0) return 0;
  return static_cast<uint32_t>((value * scale + (round ? sum / 2 : 0)) / sum);
}

uint32_t lighten(uint32_t argb) {
  auto up = [](uint8_t c) { return static_cast<uint8_t>(c + (255 - c) / 2); };
  return make_color(up(red_of(argb)), up(green_of(argb)), up(blue_of(argb)));
}

}  // namespace

int32_t PieChart::add_slice(StringView label, uint64_t value, uint32_t color) {
  UiGuard lock(this);
  if (color == 0) color = kPalette[static_cast<uint32_t>(labels_.count()) % kPaletteSize];
  if (!values_.push_back(value)) return -1;
  if (!colors_.push_back(color)) {
    values_.remove_at(values_.count() - 1);
    return -1;
  }
  const int32_t index = labels_.add(label);
  if (index < 0) {
    values_.remove_at(values_.count() - 1);
    colors_.remove_at(colors_.count() - 1);
    return -1;
  }
  invalidate();
  return index;
}

void PieChart::clear_slices() {
  UiGuard lock(this);
  labels_.clear();
  values_.clear();
  colors_.clear();
  selected_ = -1;
  invalidate();
}

void PieChart::set_slice_value(int32_t index, uint64_t value) {
  UiGuard lock(this);
  uint64_t* v = values_.get(static_cast<uint64_t>(index));
  if (!v || index < 0) return;
  *v = value;
  invalidate();
}

uint64_t PieChart::total() const {
  uint64_t sum = 0;
  for (const uint64_t v : values_.view()) sum = v > UINT64_MAX - sum ? UINT64_MAX : sum + v;
  return sum;
}

uint32_t PieChart::permille(int32_t index) const {
  const uint64_t sum = total();
  const uint64_t* v = index >= 0 ? values_.get(static_cast<uint64_t>(index)) : nullptr;
  return v && sum ? share(*v, sum, 1000, true) : 0;
}

int32_t PieChart::slice_at_angle(uint32_t turn) const {
  turn %= kTurn;
  const uint64_t sum = total();
  if (sum == 0) return -1;
  // The last slice with a value whose start (its share of the values before it) is at or before the angle.
  int32_t found = -1, i = 0;
  uint64_t before = 0;
  for (const uint64_t v : values_.view()) {
    if (v != 0 && share(before, sum, kTurn, false) <= turn) found = i;
    before = v > UINT64_MAX - before ? UINT64_MAX : before + v;   // saturates like total(), so a prefix never passes `sum`
    ++i;
  }
  return found;
}

Rect2D PieChart::pie_rect() const {
  const int32_t w = static_cast<int32_t>(bounds_.width), h = static_cast<int32_t>(bounds_.height);
  int32_t room_w = w - 2 * kMargin;
  if (legend_ && slice_count() > 0) {
    const int32_t lw = legend_width();
    room_w -= lw + kMargin;
  }
  const int32_t room_h = h - 2 * kMargin;
  int32_t d = room_w < room_h ? room_w : room_h;
  if (d < 4) return {0, 0, 0, 0};
  return {kMargin, (h - d) / 2, static_cast<uint32_t>(d), static_cast<uint32_t>(d)};
}

int32_t PieChart::slice_at(int32_t x, int32_t y) const {
  const Rect2D r = pie_rect();
  if (r.width == 0) return -1;
  const int64_t d = r.width;
  const int64_t dx = int64_t{2} * (x - r.x) + 1 - d, dy = int64_t{2} * (y - r.y) + 1 - d;   // doubled, from the centre
  if (dx * dx + dy * dy > d * d) return -1;
  return slice_at_angle(angle_of(static_cast<int32_t>(dx), static_cast<int32_t>(dy)));
}

void PieChart::legend_text(int32_t index, String& out) const {
  out.assign(labels_.at(index));
  out.append("  ");
  const uint32_t pm = permille(index);
  out.append_uint(pm / 10);
  out.append(".");
  out.append_uint(pm % 10);
  out.append("%");
}

int32_t PieChart::legend_width() const {
  int32_t widest = 0;
  String text(allocator());
  for (int32_t i = 0; i < slice_count(); ++i) {
    legend_text(i, text);
    const int32_t tw = Painter::text_width(text.view());
    if (tw > widest) widest = tw;
  }
  const int32_t wanted = kSwatch + 6 + widest;
  const int32_t cap = static_cast<int32_t>(bounds_.width) / 2;   // never more than half the control
  return wanted < cap ? wanted : cap;
}

int32_t PieChart::legend_row_at(int32_t x, int32_t y) const {
  if (!legend_ || slice_count() == 0) return -1;
  const int32_t lw = legend_width();
  const int32_t left = static_cast<int32_t>(bounds_.width) - kMargin - lw;
  const int32_t top = (static_cast<int32_t>(bounds_.height) - slice_count() * kRowHeight) / 2;
  if (x < left || x >= left + lw || y < top) return -1;
  const int32_t row = (y - top) / kRowHeight;
  return row < slice_count() ? row : -1;
}

void PieChart::select(int32_t index) {
  if (index == selected_) return;
  selected_ = index;
  invalidate();
  fire(changed_);
}

void PieChart::set_selected_index(int32_t index) {
  UiGuard lock(this);
  select(index >= 0 && index < slice_count() ? index : -1);
}

void PieChart::paint(Painter& painter) {
  const Theme& t = painter.theme();
  const Rect2D all{0, 0, bounds_.width, bounds_.height};
  painter.fill(all, t.face);
  const Rect2D r = pie_rect();
  const uint64_t sum = total();
  const int32_t n = slice_count();
  if (r.width > 0) {
    const int32_t d = static_cast<int32_t>(r.width);
    // Where each slice starts, so that a pixel only needs a short scan (slices are few).
    for (int32_t row = 0; row < d; ++row) {
      const int64_t dy = int64_t{2} * row + 1 - d;
      int32_t run_start = -1, run_slice = -2;   // -1 is "no slice" (an empty chart)
      auto flush = [&](int32_t end) {
        if (run_start < 0) return;
        uint32_t color = t.field;
        if (run_slice >= 0) {
          color = slice_color(run_slice);
          if (run_slice == selected_) color = lighten(color);
        }
        painter.fill({r.x + run_start, r.y + row, static_cast<uint32_t>(end - run_start), 1}, color);
        run_start = -1;
      };
      for (int32_t col = 0; col < d; ++col) {
        const int64_t dx = int64_t{2} * col + 1 - d;
        if (dx * dx + dy * dy > int64_t{d} * d) {
          flush(col);
          continue;
        }
        const int32_t s = sum ? slice_at_angle(angle_of(static_cast<int32_t>(dx), static_cast<int32_t>(dy))) : -1;
        if (run_start >= 0 && s != run_slice) flush(col);
        if (run_start < 0) { run_start = col; run_slice = s; }
      }
      flush(d);
    }
    // The rim: the circle drawn once more as a one pixel ring in the dark colour.
    for (int32_t row = 0; row < d; ++row) {
      const int64_t dy = int64_t{2} * row + 1 - d;
      for (int32_t col = 0; col < d; ++col) {
        const int64_t dx = int64_t{2} * col + 1 - d;
        const int64_t dist = dx * dx + dy * dy;
        const int64_t inner = int64_t{d - 2} * (d - 2);
        if (dist <= int64_t{d} * d && dist > inner) painter.fill({r.x + col, r.y + row, 1, 1}, t.dark);
      }
    }
  }
  if (legend_ && n > 0) {
    const int32_t lw = legend_width();
    const int32_t left = static_cast<int32_t>(bounds_.width) - kMargin - lw;
    const int32_t top = (static_cast<int32_t>(bounds_.height) - n * kRowHeight) / 2;
    String text(allocator());
    for (int32_t i = 0; i < n; ++i) {
      const int32_t y = top + i * kRowHeight;
      const bool sel = i == selected_;
      if (sel) painter.fill({left, y, static_cast<uint32_t>(lw), static_cast<uint32_t>(kRowHeight)}, t.selection);
      painter.fill({left + 1, y + (kRowHeight - kSwatch) / 2, static_cast<uint32_t>(kSwatch), static_cast<uint32_t>(kSwatch)}, slice_color(i));
      painter.outline({left + 1, y + (kRowHeight - kSwatch) / 2, static_cast<uint32_t>(kSwatch), static_cast<uint32_t>(kSwatch)}, t.dark);
      legend_text(i, text);
      // The row is clipped to the legend, so an over-long label is cut off rather than spilling over the pie.
      painter.push_frame({left + kSwatch + 5, y, static_cast<uint32_t>(lw - kSwatch - 5), static_cast<uint32_t>(kRowHeight)});
      painter.text(0, (kRowHeight - Painter::kGlyphHeight) / 2, text.view(), sel ? t.selection_text : t.text);
      painter.pop_frame();
    }
  }
  if (focused()) painter.focus_frame(all);
}

void PieChart::on_mouse_down(const MouseArgs& args) {
  if (args.button != MouseButton::Left) return;
  int32_t hit = legend_row_at(args.x, args.y);
  if (hit < 0) hit = slice_at(args.x, args.y);
  select(hit);
}

bool PieChart::on_key(const KeyArgs& args) {
  const int32_t n = slice_count();
  if (n == 0) return false;
  if (args.usage == hardware::key::kDown) {
    select(selected_ + 1 < n ? selected_ + 1 : 0);
    return true;
  }
  if (args.usage == hardware::key::kUp) {
    select(selected_ > 0 ? selected_ - 1 : n - 1);
    return true;
  }
  return false;
}

}  // namespace gui
