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

// What a laid out page looks like as a list of drawing operations in page coordinates (CSS pixels, the origin at the top left of the page, y down),
// in the order they are painted: backgrounds, borders, shadows, text, pictures, with the clips and opacity layers around them that overflow and
// opacity make. build_display_list() (PaintBuilder.hpp) makes it from a laid out box tree following the stacking order of CSS 2.1 appendix E;
// DisplayPainter (DisplayPainter.hpp) draws it. Every operation has the rectangle it covers, so a painter can skip what is off the screen.
#pragma once
#include "Font/FontSet.hpp"
#include "Graphics/Graphics2D/IRenderer2D.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/String.hpp"

namespace paint {

// A rectangle in whole pixels.
struct IRect {
  int32_t x = 0, y = 0, width = 0, height = 0;
  int32_t right() const { return x + width; }
  int32_t bottom() const { return y + height; }
  bool empty() const { return width <= 0 || height <= 0; }
  bool intersects(const IRect& o) const { return !empty() && !o.empty() && x < o.right() && o.x < right() && y < o.bottom() && o.y < bottom(); }
  IRect intersection(const IRect& o) const {
    const int32_t l = x > o.x ? x : o.x, t = y > o.y ? y : o.y;
    const int32_t r = right() < o.right() ? right() : o.right(), b = bottom() < o.bottom() ? bottom() : o.bottom();
    IRect out;
    out.x = l;
    out.y = t;
    out.width = r > l ? r - l : 0;
    out.height = b > t ? b - t : 0;
    return out;
  }
  bool contains(int32_t px, int32_t py) const { return px >= x && py >= y && px < right() && py < bottom(); }
};

enum class OpKind : uint8_t {
  FillRect,       // `rect` in `color` (rounded when `radii` is not all 0)
  Border,         // the border of `rect`: widths, colours and styles per side, rounded by `radii`
  Gradient,       // `rect` filled with gradient number `gradient`, rounded by `radii`
  Text,           // a run of text: see the text fields
  Object,         // a picture, video, canvas or svg of the element `node`, drawn by the object painter into `rect`
  PushClip,       // everything until the matching PopClip is clipped to `rect` (rounded by `radii`)
  PopClip,
  PushLayer,      // everything until the matching PopLayer is drawn as one group with `opacity` (0..255)
  PopLayer,
  Hit,            // nothing is drawn: the element `node` is under `rect` (what a click or the pointer finds), with the cursor it asks for
};

// Border line styles (browser::BorderStyle's order).
enum class LineStyle : uint8_t { None, Hidden, Dotted, Dashed, Solid, Double, Groove, Ridge, Inset, Outset };

constexpr uint8_t kDecorationUnderline = 1, kDecorationStrike = 2, kDecorationOverline = 4;

struct Op {
  OpKind kind = OpKind::FillRect;
  uint8_t opacity = 255;                 // PushLayer
  uint8_t decorations = 0;               // Text: kDecoration* bits
  uint8_t cursor = 0;                    // Hit: a browser::Cursor
  IRect rect;                            // where it draws (Text: the content area of the run, for hit testing and culling)
  uint32_t color = 0;                    // FillRect, Text: 0xAARRGGBB
  graphics::CornerRadii radii;           // FillRect, Border, Gradient, PushClip
  uint32_t node = 0;                     // Text and Object: the serial of the element it belongs to (0: none)

  // Border
  uint32_t border_colors[4] = {0, 0, 0, 0};          // top, right, bottom, left
  uint16_t border_widths[4] = {0, 0, 0, 0};
  LineStyle border_styles[4] = {LineStyle::None, LineStyle::None, LineStyle::None, LineStyle::None};

  // Gradient
  uint32_t gradient = 0;                 // an index into DisplayList::gradient()

  // Text: the characters (a slice of the list's text pool), the font, and where the first glyph sits: x in 1/64 px, baseline in whole pixels
  uint32_t text_offset = 0, text_length = 0;
  font::FontHandle font;
  int32_t text_x64 = 0, baseline = 0;
  int32_t letter_spacing64 = 0, word_spacing64 = 0;
  int32_t decoration_thickness = 1, decoration_offset = 0;   // underline: pixels below the baseline; strike: pixels above it (negative)

  // Object
  uint8_t object_kind = 0;               // browser::ObjectKind
};

// A gradient fill ready to draw: a line (linear) or an ellipse (radial) in page coordinates, and the stops.
struct GradientData {
  bool radial = false;
  graphics::Vec2 from, to;               // linear: the gradient line
  graphics::Vec2 center, radii;          // radial
  uint32_t stop_count = 0;
  graphics::GradientStop stops[graphics::kMaxGradientStops];
};

class DisplayList {
 public:
  explicit DisplayList(allocators::IAllocator& allocator) : ops_(allocator), text_(allocator), gradients_(allocator) {}
  DisplayList(const DisplayList&) = delete;
  DisplayList& operator=(const DisplayList&) = delete;

  void clear() {
    ops_.clear();
    text_.clear();
    gradients_.clear();
    width_ = height_ = 0;
    background_ = 0;
  }

  uint64_t count() const { return ops_.count(); }
  const Op& op(uint64_t index) const { return ops_[index]; }
  StringView text(const Op& op) const { return text_.view().substr(op.text_offset, op.text_length); }
  const GradientData& gradient(uint32_t index) const { return gradients_[index]; }
  uint64_t gradient_count() const { return gradients_.count(); }

  // The size of the page's content in pixels and the colour of the page's canvas (0: none given).
  int32_t width() const { return width_; }
  int32_t height() const { return height_; }
  uint32_t background() const { return background_; }
  void set_extent(int32_t width, int32_t height) {
    width_ = width;
    height_ = height;
  }
  void set_background(uint32_t color) { background_ = color; }

  // Appending (false if memory ran out).
  bool add(const Op& op) { return ops_.push_back(op); }
  // Keeps `characters` in the text pool and returns where (for Op::text_offset); false if memory ran out.
  bool add_text(StringView characters, uint32_t* offset) {
    *offset = static_cast<uint32_t>(text_.size());
    return text_.append(characters);
  }
  bool add_gradient(const GradientData& g, uint32_t* index) {
    *index = static_cast<uint32_t>(gradients_.count());
    return gradients_.push_back(g);
  }

 private:
  List<Op> ops_;
  String text_;
  List<GradientData> gradients_;
  int32_t width_ = 0, height_ = 0;
  uint32_t background_ = 0;
};

}  // namespace paint
