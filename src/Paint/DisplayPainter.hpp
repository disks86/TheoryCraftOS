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

// Draws a display list (DisplayList.hpp) with an IRenderer2D: rectangles, rounded borders, gradients, anti-aliased text from the font stack, and the
// pictures, videos and canvases that the page's object painter draws. Only what falls inside the view is drawn.
// Groups (transforms, filters) are drawn into an offscreen picture of the visible part with the software renderer, and moved or filtered by the code in
// Raster.hpp (a translation by whole pixels needs no picture); nothing is kept from one paint to the next.
#pragma once
#include "Browser/ObjectTypes.hpp"
#include "Font/FontSet.hpp"
#include "Paint/DisplayList.hpp"

namespace paint {

class DisplayPainter {
 public:
  DisplayPainter(allocators::IAllocator& allocator, font::FontSet& fonts, browser::IObjectPainter* objects)
      : allocator_(allocator), fonts_(fonts), objects_(objects), resolved_(allocator), have_(allocator) {}

  // Draws `list` so that page position (0, 0) is at (dx, dy) on the target, inside `view` (target coordinates; nothing outside it changes).
  // The canvas colour of the page is not drawn here: fill the view with DisplayList::background() (or the default paper) first.
  // Returns false if some clip or layer could not be done exactly (the picture is still drawn).
  bool paint(graphics::IRenderer2D& renderer, const DisplayList& list, int32_t dx, int32_t dy, const graphics::Rect2D& view);

  // Draws a run of text (used by paint(), and by controls that draw their own): the pen starts at x64 (1/64 px) on `baseline`.
  void draw_text(graphics::IRenderer2D& renderer, font::FontHandle font, StringView text, int32_t x64, int32_t baseline, uint32_t color, int32_t letter_spacing64,
                 int32_t word_spacing64);

 private:
  // The operations [begin, end) of the list drawn at offset (dx, dy), culled against `view`. `groups` counts the offscreen groups around this call.
  bool run(graphics::IRenderer2D& renderer, const DisplayList& list, uint64_t begin, uint64_t end, int32_t dx, int32_t dy, const IRect& view, uint32_t groups);
  // A PushTransform / PushFilter at `begin` whose pop is at `close`: its content goes to an offscreen picture that is moved or filtered and drawn back.
  bool paint_group(graphics::IRenderer2D& renderer, const DisplayList& list, uint64_t begin, uint64_t close, int32_t dx, int32_t dy, const IRect& visible, uint32_t groups);
  bool push_clip_shape(graphics::IRenderer2D& renderer, const ClipShape& shape, int32_t dx, int32_t dy, const IRect& area);
  void draw_border(graphics::IRenderer2D& renderer, const Op& op, const IRect& rect, const IRect& visible);
  void draw_gradient(graphics::IRenderer2D& renderer, const DisplayList& list, const Op& op, const IRect& rect, int32_t dx, int32_t dy, const IRect& visible);
  void draw_shadow(graphics::IRenderer2D& renderer, const ShadowData& shadow, const IRect& bounds, int32_t dx, int32_t dy, const IRect& visible);
  void draw_image_layer(graphics::IRenderer2D& renderer, const DisplayList& list, const Op& op, int32_t dx, int32_t dy, const IRect& visible);
  // A text op: the run and its decorations, blurred when it is a text-shadow.
  void draw_text_op(graphics::IRenderer2D& renderer, const DisplayList& list, const Op& op, const IRect& rect, int32_t dx, int32_t dy, const IRect& visible);
  void draw_text_run(graphics::IRenderer2D& renderer, const DisplayList& list, const Op& op, const IRect& rect, int32_t dx, int32_t dy, uint32_t color);
  void draw_decoration(graphics::IRenderer2D& renderer, int32_t x, int32_t y, int32_t width, int32_t thickness, DecorationStyle style, uint32_t color, bool upward);
  // The font of a list's font table in this painter's font set (matched on first use during a paint).
  font::FontHandle font_of(const DisplayList& list, uint32_t index);
  List<font::FontHandle> resolved_;
  List<uint8_t> have_;

  allocators::IAllocator& allocator_;
  font::FontSet& fonts_;
  browser::IObjectPainter* objects_;
};

}  // namespace paint
