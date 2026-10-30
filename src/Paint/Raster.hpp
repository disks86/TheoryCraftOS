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

// The pixel work behind the painter's groups: offscreen pixmaps and coverage masks (allocator backed), anti-aliased rounded rectangles and polygons,
// Gaussian blur (three box blurs), the colour filters of CSS, and an affine resampler. Nothing here knows about the display list's drawing order.
#pragma once
#include "Allocators/IAllocator.hpp"
#include "Paint/DisplayList.hpp"
#include "Utility/AllocatedView.hpp"

namespace paint {

// A picture of 0xAARRGGBB pixels (straight alpha) in memory of the allocator.
class Pixmap {
 public:
  static constexpr uint64_t kMaxPixels = uint64_t{1} << 24;
  explicit Pixmap(allocators::IAllocator& allocator) : allocator_(allocator) {}
  ~Pixmap() { release_view(allocator_, pixels_); }
  Pixmap(const Pixmap&) = delete;
  Pixmap& operator=(const Pixmap&) = delete;

  // A transparent picture of that size; false if it is empty, too big or memory ran out.
  bool create(int32_t width, int32_t height);
  int32_t width() const { return width_; }
  int32_t height() const { return height_; }
  ArrayView<uint32_t> pixels() const { return pixels_; }
  graphics::Surface surface() const;
  allocators::IAllocator& allocator() const { return allocator_; }

 private:
  allocators::IAllocator& allocator_;
  ArrayView<uint32_t> pixels_;
  int32_t width_ = 0, height_ = 0;
};

// 8-bit coverage, 0 nothing and 255 everything.
class AlphaMask {
 public:
  explicit AlphaMask(allocators::IAllocator& allocator) : allocator_(allocator) {}
  ~AlphaMask() { release_view(allocator_, data_); }
  AlphaMask(const AlphaMask&) = delete;
  AlphaMask& operator=(const AlphaMask&) = delete;

  bool create(int32_t width, int32_t height);   // all 0
  int32_t width() const { return width_; }
  int32_t height() const { return height_; }
  ArrayView<uint8_t> data() const { return data_; }
  graphics::MaskView view() const;
  allocators::IAllocator& allocator() const { return allocator_; }

 private:
  allocators::IAllocator& allocator_;
  ArrayView<uint8_t> data_;
  int32_t width_ = 0, height_ = 0;
};

// A rounded rectangle prepared for coverage queries (floating point rectangle, radii already scaled down the CSS way when they do not fit).
struct RRect {
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  float tlx = 0, tly = 0, trx = 0, try_ = 0, brx = 0, bry = 0, blx = 0, bly = 0;
  static RRect make(float x, float y, float width, float height, const graphics::CornerRadii& radii);
  bool empty() const { return !(x1 > x0 && y1 > y0); }
  // The share of pixel (px, py) the shape covers, 0..1 (anti-aliased on the curves).
  float coverage(int32_t px, int32_t py) const;
  // How far the point is inside the shape (negative outside), measured to the nearest edge; approximate on the curves.
  float depth(float x, float y) const;
};

// Sets mask pixels to the larger of what they were and the coverage of the shape (the mask's top left is page position (ox, oy)).
void fill_rrect(AlphaMask& mask, int32_t ox, int32_t oy, const RRect& shape);
// The same for a polygon (even-odd or non-zero); anti-aliased with four sub-scanlines per pixel.
void fill_polygon(AlphaMask& mask, int32_t ox, int32_t oy, ArrayView<const graphics::Vec2> points, bool evenodd);
// Is the point inside the shape of a clip-path?
bool shape_contains(const ClipShape& shape, double x, double y);
// The shape's bounds in whole pixels.
IRect shape_bounds(const ClipShape& shape);

// How many pixels around a blurred shape the blur reaches (0 for a sigma too small to matter).
int32_t blur_reach(float sigma);
// Gaussian blur with that standard deviation (three box blurs; the outside counts as transparent). False if memory ran out.
bool blur_mask(AlphaMask& mask, float sigma);
bool blur_pixmap(Pixmap& pixmap, float sigma);

// One filter function on a picture (everything but Blur and DropShadow, which need room: blur_pixmap and drop_shadow).
void color_filter(Pixmap& pixmap, FilterStep::Kind kind, float amount);
// The same filter function as a colour transform for the renderer's group filters; false for a kind that is not one (Blur, DropShadow).
bool color_filter_transform(FilterStep::Kind kind, float amount, graphics::ColorTransform* out);
// Draws a shadow of the picture under it: the alpha shifted by (dx, dy), blurred with `sigma` and tinted with `color`. False if memory ran out.
bool drop_shadow(Pixmap& pixmap, int32_t dx, int32_t dy, float sigma, uint32_t color);

// Draws `source` (whose top left sits at (source_x, source_y) in target coordinates) as `transform` moves it, into a new picture covering `dest`
// (target coordinates). Axis aligned transforms use a box filter (a translation by whole pixels or a scale that lands on pixel edges stays exact);
// others sample bilinearly (with more samples when they shrink). Outside the source is transparent. False if memory ran out.
bool resample(const Pixmap& source, double source_x, double source_y, const Affine& transform, const IRect& dest, Pixmap* out);

// Straight alpha `src` over `dst`.
uint32_t blend_over(uint32_t src, uint32_t dst);

// The smallest rectangle with whole pixel edges holding the transformed rectangle (clamped to a sane range).
IRect transformed_bounds(const Affine& transform, const IRect& rect);

}  // namespace paint
