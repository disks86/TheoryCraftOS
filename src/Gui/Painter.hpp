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

// Drawing for controls: an IRenderer2D with a coordinate origin and clip that
// follow the control being painted, plus the bevels and text helpers the
// classic look needs. Controls draw in their own coordinates (0,0 = top left).
#pragma once
#include "Graphics/Graphics2D/IRenderer2D.hpp"
#include "Gui/GuiTypes.hpp"
#include "Gui/Theme.hpp"
#include "Utility/StringView.hpp"

namespace gui {

class Painter {
 public:
  static constexpr int32_t kGlyphWidth = 8, kGlyphHeight = 16;

  Painter(graphics::IRenderer2D& renderer, const Theme& theme);

  const Theme& theme() const { return theme_; }
  graphics::IRenderer2D& renderer() { return renderer_; }

  // Enters a child area: `bounds` is in the current coordinates; drawing is then relative to its
  // top left and clipped to it (and to everything outside). Pair with pop_frame().
  void push_frame(const Rect2D& bounds);
  void pop_frame();
  bool clip_empty() const;

  void fill(const Rect2D& r, uint32_t color);
  void outline(const Rect2D& r, uint32_t color);
  void line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t color);
  // A two-pixel 3D edge; `sunken` swaps the light and dark sides. Leaves the inside (r shrunk by 2) alone.
  void bevel(const Rect2D& r, bool sunken);
  void text(int32_t x, int32_t y, StringView text, uint32_t color);
  // Centered in `r`, both ways.
  void text_centered(const Rect2D& r, StringView text, uint32_t color);
  // A dotted-looking focus frame.
  void focus_frame(const Rect2D& r);

  static int32_t text_width(StringView text) { return static_cast<int32_t>(text.size()) * kGlyphWidth; }

 private:
  struct Frame {
    int32_t origin_x, origin_y;
    Rect2D clip;  // absolute
  };
  static constexpr uint32_t kMaxDepth = 48;

  void apply_clip();
  Rect2D absolute(const Rect2D& r) const;

  graphics::IRenderer2D& renderer_;
  const Theme& theme_;
  Frame frames_[kMaxDepth];
  uint32_t depth_ = 0;  // frames_[depth_] is current
};

}  // namespace gui
