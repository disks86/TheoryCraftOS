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

#include "Gui/Controls/WordCloud.hpp"
#include "Hardware/GraphicsProcessingUnit/Font8x16.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"

namespace gui {

namespace {

constexpr uint32_t kPalette[] = {0xFF4E79A7, 0xFFF28E2B, 0xFFE15759, 0xFF76B7B2, 0xFF59A14F, 0xFFEDC948, 0xFFB07AA1, 0xFF9C755F};
constexpr uint32_t kPaletteSize = sizeof(kPalette) / sizeof(kPalette[0]);
constexpr int64_t kMaxWordWidth = 1 << 20;  // keeps the layout arithmetic far from overflow

// The text with each font pixel drawn as a scale x scale square (runs of set bits in a row are one rectangle).
void draw_scaled(Painter& painter, int32_t x, int32_t y, StringView text, int32_t scale, uint32_t color) {
  const int32_t s = scale;
  int32_t pen = x;
  for (const char c : text) {
    const ConstByteView glyph = hardware::Font8x16::glyph(c);
    for (uint32_t row = 0; row < hardware::Font8x16::kHeight && row < glyph.size(); ++row) {
      const uint8_t bits = glyph[row];
      uint32_t col = 0;
      while (col < hardware::Font8x16::kWidth) {
        if (!(bits & (0x80u >> col))) {
          ++col;
          continue;
        }
        uint32_t end = col;
        while (end < hardware::Font8x16::kWidth && (bits & (0x80u >> end))) ++end;
        painter.fill({pen + static_cast<int32_t>(col) * s, y + static_cast<int32_t>(row) * s, static_cast<uint32_t>(end - col) * static_cast<uint32_t>(s),
                      static_cast<uint32_t>(s)},
                     color);
        col = end;
      }
    }
    pen += static_cast<int32_t>(hardware::Font8x16::kWidth) * s;
  }
}

}  // namespace

int32_t WordCloud::add_word(StringView text, uint64_t weight, uint32_t color) {
  UiGuard lock(this);
  if (color == 0) color = kPalette[static_cast<uint32_t>(words_.count()) % kPaletteSize];
  if (!weights_.push_back(weight)) return -1;
  if (!colors_.push_back(color)) {
    weights_.remove_at(weights_.count() - 1);
    return -1;
  }
  const int32_t index = words_.add(text);
  if (index < 0) {
    weights_.remove_at(weights_.count() - 1);
    colors_.remove_at(colors_.count() - 1);
    return -1;
  }
  touch();
  return index;
}

void WordCloud::clear_words() {
  UiGuard lock(this);
  words_.clear();
  weights_.clear();
  colors_.clear();
  selected_ = -1;
  touch();
}

void WordCloud::set_word_weight(int32_t index, uint64_t weight) {
  UiGuard lock(this);
  uint64_t* w = index >= 0 ? weights_.get(static_cast<uint64_t>(index)) : nullptr;
  if (!w) return;
  *w = weight;
  touch();
}

int32_t WordCloud::word_scale(int32_t index) const {
  const int32_t n = word_count();
  if (index < 0 || index >= n) return 1;
  uint64_t low = UINT64_MAX, high = 0;
  for (const uint64_t w : weights_.view()) {
    low = w < low ? w : low;
    high = w > high ? w : high;
  }
  if (high == low) return 2;
  // Rounded to the nearest step: 0 .. kMaxScale-1 steps above 1x.
  const uint64_t w = word_weight(index);
  const uint64_t span = high - low, off = w - low;
  const uint32_t steps = kMaxScale - 1;
  uint64_t scaled_off = off, scaled_span = span;
  while (scaled_span > UINT64_MAX / (2 * steps)) {  // halve until off * 2 * steps fits
    scaled_span >>= 1;
    scaled_off >>= 1;
  }
  const uint64_t step = scaled_span ? (scaled_off * 2 * steps + scaled_span) / (2 * scaled_span) : 0;
  return 1 + static_cast<int32_t>(step < steps ? step : steps);
}

void WordCloud::ensure_layout() const {
  const uint32_t width = bounds_.width;
  if (layout_valid_ && laid_width_ == width) return;
  layout_valid_ = false;
  rects_.clear();
  const int32_t n = word_count();
  const int64_t avail = static_cast<int64_t>(width) - 2 * kMargin;
  if (!rects_.reserve(static_cast<uint64_t>(n))) return;
  int32_t line_start = 0;
  int64_t line_width = 0;
  int32_t y = kMargin;

  // Places words [line_start, end) centred on the line starting at y; returns the line's height.
  auto flush = [&](int32_t end) {
    int32_t height = 0;
    for (int32_t i = line_start; i < end; ++i) {
      const int32_t h = Painter::kGlyphHeight * word_scale(i);
      height = h > height ? h : height;
    }
    int64_t x = kMargin + (avail > line_width ? (avail - line_width) / 2 : 0);
    for (int32_t i = line_start; i < end; ++i) {
      const int32_t s = word_scale(i);
      int64_t w = int64_t{Painter::kGlyphWidth} * s * static_cast<int64_t>(word_text(i).size());
      w = w < kMaxWordWidth ? w : kMaxWordWidth;
      const int32_t h = Painter::kGlyphHeight * s;
      rects_.push_back({static_cast<int32_t>(x), y + (height - h) / 2, static_cast<uint32_t>(w), static_cast<uint32_t>(h)});
      x += w + kWordGap;
    }
    y += height + kLineGap;
  };

  for (int32_t i = 0; i < n; ++i) {
    int64_t w = int64_t{Painter::kGlyphWidth} * word_scale(i) * static_cast<int64_t>(word_text(i).size());
    w = w < kMaxWordWidth ? w : kMaxWordWidth;
    const int64_t needed = line_width + (i > line_start ? kWordGap : 0) + w;
    if (i > line_start && needed > avail) {
      flush(i);
      line_start = i;
      line_width = w;
    } else {
      line_width = needed;
    }
  }
  if (n > 0) flush(n);
  laid_width_ = width;
  layout_valid_ = true;
}

Rect2D WordCloud::word_rect(int32_t index) const {
  ensure_layout();
  const Rect2D* r = layout_valid_ && index >= 0 ? rects_.get(static_cast<uint64_t>(index)) : nullptr;
  return r ? *r : Rect2D{0, 0, 0, 0};
}

int32_t WordCloud::word_at(int32_t x, int32_t y) const {
  ensure_layout();
  if (!layout_valid_) return -1;
  for (int32_t i = word_count() - 1; i >= 0; --i) {
    if (contains(word_rect(i), x, y)) return i;
  }
  return -1;
}

void WordCloud::select(int32_t index) {
  if (index == selected_) return;
  selected_ = index;
  invalidate();
  fire(changed_);
}

void WordCloud::set_selected_index(int32_t index) {
  UiGuard lock(this);
  select(index >= 0 && index < word_count() ? index : -1);
}

void WordCloud::paint(Painter& painter) {
  const Theme& t = painter.theme();
  const Rect2D all{0, 0, bounds_.width, bounds_.height};
  painter.fill(all, t.field);
  painter.bevel(all, true);
  painter.push_frame({2, 2, bounds_.width > 4 ? bounds_.width - 4 : 0, bounds_.height > 4 ? bounds_.height - 4 : 0});
  // Words are laid out in the control's coordinates; the frame moves the origin by its 2 pixel inset, so they are drawn 2 up and left.
  for (int32_t i = 0; i < word_count(); ++i) {
    const Rect2D r = word_rect(i);
    if (r.width == 0) continue;
    const bool chosen = i == selected_;
    if (chosen) painter.fill({r.x - 2 - 2, r.y - 2, r.width + 4, r.height}, t.selection);
    draw_scaled(painter, r.x - 2, r.y - 2, word_text(i), word_scale(i), chosen ? t.selection_text : word_color(i));
  }
  painter.pop_frame();
  if (focused()) painter.focus_frame(all);
}

void WordCloud::on_mouse_down(const MouseArgs& args) {
  if (args.button != MouseButton::Left) return;
  select(word_at(args.x, args.y));
}

bool WordCloud::on_key(const KeyArgs& args) {
  const int32_t n = word_count();
  if (n == 0) return false;
  if (args.usage == hardware::key::kRight) {
    select(selected_ + 1 < n ? selected_ + 1 : 0);
    return true;
  }
  if (args.usage == hardware::key::kLeft) {
    select(selected_ > 0 ? selected_ - 1 : n - 1);
    return true;
  }
  return false;
}

}  // namespace gui
