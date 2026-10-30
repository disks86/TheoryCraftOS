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

// 2D rendering: shapes, images and text onto one render target. Colors are
// 0xAARRGGBB with straight alpha; coordinates are pixels from the top left and
// everything is clipped to the target and the current clip rectangle.
// Implementations: SoftwareRenderer2D (CPU, works everywhere) and, when a GPU
// with a 3D engine is present, the accelerated one picked by SystemGraphics.
#pragma once
#include "Graphics/GraphicsTypes.hpp"

namespace graphics {

class IGraphicsDevice;
class IImage;

enum class BlendMode : uint8_t {
  Replace,  // write the source color as is
  Alpha,    // source over destination using the source alpha
};

// Radii of the four corners of a rounded rectangle, as ellipse half axes in pixels (tl = top left, tr = top
// right, br = bottom right, bl = bottom left). Negative values count as 0; if the radii of two corners on
// one side add up to more than that side, all radii are scaled down together (the CSS rule).
struct CornerRadii {
  float tl_x = 0, tl_y = 0, tr_x = 0, tr_y = 0, br_x = 0, br_y = 0, bl_x = 0, bl_y = 0;
  static constexpr CornerRadii uniform(float r) { return {r, r, r, r, r, r, r, r}; }
};

// Border widths in pixels, one per side.
struct EdgeWidths {
  uint32_t top = 0, right = 0, bottom = 0, left = 0;
  static constexpr EdgeWidths uniform(uint32_t w) { return {w, w, w, w}; }
};

// A rectangle with fractional position and size (the source of a scaled image).
struct RectF {
  float x = 0, y = 0, width = 0, height = 0;
};

// One color stop of a gradient: `offset` 0..1 along the gradient line (clamped, and never less than the
// offset before it), `color` 0xAARRGGBB straight alpha. Colors are interpolated with premultiplied alpha.
struct GradientStop {
  float offset = 0;
  uint32_t color = 0;
};

// An affine map of the plane: (x, y) -> (a x + c y + e, b x + d y + f).
struct Matrix2D {
  float a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
};

// A colour filter function on straight (not premultiplied) colours, channels 0..1: rgb' = m * rgb + add (m row by row), alpha' = alpha * alpha_scale; results are clamped.
struct ColorTransform {
  float m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  float add[3] = {0, 0, 0};
  float alpha_scale = 1;
};

enum class ImageFilter : uint8_t {
  Nearest,   // the texel containing the pixel center
  Bilinear,  // 2x2 texels weighted by distance, alpha-aware, never reading outside the source rectangle
};

// A read-only 8-bit coverage bitmap (0 = nothing, 255 = fully covered), e.g. a rendered glyph. `pitch` is the
// distance between rows in bytes.
struct MaskView {
  ConstByteView coverage;
  uint32_t width = 0, height = 0, pitch = 0;
  bool valid() const { return width && height && pitch >= width && coverage.size() >= uint64_t{pitch} * height; }
};

constexpr uint32_t kMaxGradientStops = 32;  // further stops are ignored
constexpr uint32_t kMaxClipDepth = 32;
constexpr uint32_t kMaxLayerDepth = 8;

class IRenderer2D {
 public:
  virtual const char* name() const = 0;
  // True if the drawing is done by a GPU rather than the CPU.
  virtual bool accelerated() const = 0;
  virtual Extent2D extent() const = 0;

  virtual void set_clip(const Rect2D& clip) = 0;
  virtual void reset_clip() = 0;
  virtual void set_blend(BlendMode mode) = 0;

  virtual void clear(uint32_t color) = 0;
  virtual void put_pixel(int32_t x, int32_t y, uint32_t color) = 0;
  virtual void draw_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t color) = 0;
  virtual void fill_rect(const Rect2D& rect, uint32_t color) = 0;
  virtual void draw_rect(const Rect2D& rect, uint32_t color) = 0;  // 1 pixel outline
  virtual void fill_circle(int32_t cx, int32_t cy, uint32_t radius, uint32_t color) = 0;
  virtual void fill_triangle(Vec2 a, Vec2 b, Vec2 c, uint32_t color) = 0;
  // Copies `source_rect` of `source` (0xAARRGGBB through source.decode) with its top left at (x, y).
  virtual void draw_image(const Surface& source, const Rect2D& source_rect, int32_t x, int32_t y) = 0;
  // Fixed 8x16 font, no wrapping; '\n' is not interpreted. Background pixels are left alone.
  virtual void draw_text(int32_t x, int32_t y, const char* text, uint32_t color) = 0;

  // ---- Pixel-engine primitives -------------------------------------------------------------------------
  // Anti-aliased edges (rounded rectangles, masks, gradients) are coverage-weighted alpha: with
  // BlendMode::Replace there is no anti-aliasing, a pixel is written when its coverage is at least half.
  // Everything below honours the clip rectangle, the clip stack and the current layer.

  // Fills a rectangle whose corners are quarter ellipses. Straight edges are pixel exact; curved edges are
  // anti-aliased (pixel-center distance to the curve).
  virtual void fill_rounded_rect(const Rect2D& rect, const CornerRadii& radii, uint32_t color) = 0;
  // Draws the border of a rounded rectangle: the area between `outer` and the rectangle inset by `widths`
  // (whose corner radii are the outer ones minus the adjacent widths, at least 0). Sides may differ.
  virtual void stroke_rounded_rect(const Rect2D& outer, const CornerRadii& radii, const EdgeWidths& widths,
                                   uint32_t color) = 0;
  // The glyph blit: `source_rect` of `mask` tinted with `color` (its alpha scales the coverage), top left at
  // (x, y). Fully covered pixels of an opaque color are written directly.
  virtual void draw_mask(const MaskView& mask, const Rect2D& source_rect, int32_t x, int32_t y, uint32_t color) = 0;
  // Draws `source_rect` of `source` stretched over `dest`. Pixel centers of `dest` map linearly into
  // `source_rect`; samples never leave the rectangle (clamped at its edges).
  virtual void draw_image_scaled(const Surface& source, const RectF& source_rect, const Rect2D& dest, ImageFilter filter) = 0;
  // Draws `source_rect` of `source` under `m`: a point (x, y) of the rectangle, measured from its top left corner in source pixels, lands at
  // m(x, y) on the target. Pixel centers of the target map back through the inverse into the rectangle (samples never leave it; a pixel whose
  // center falls outside the transformed rectangle is not drawn, so the edges are not anti-aliased). Rotation, skew and scaling in one pass,
  // done by the GPU's texture units where there is one. Returns false, with nothing drawn, when this renderer cannot do it (the default;
  // the device renderer under a layer's opacity, or without textures) or `m` is not invertible: the caller then resamples itself.
  virtual bool draw_image_transformed(const Surface& source, const Rect2D& source_rect, const Matrix2D& m, ImageFilter filter) {
    (void)source, (void)source_rect, (void)m, (void)filter;
    return false;
  }

  // Offscreen groups. begin_group() starts a transparent picture covering `bounds` (target coordinates, clipped to the target): everything drawn until
  // the matching end_group() is drawn into it, with the same coordinates and the clip stack of the group alone (pushes left open are dropped), and
  // nothing reaches the target. end_group() then lays the picture over the target: a point (x, y) of it, in the coordinates it was drawn in, lands at
  // m(x, y), filtered by `filter`, the whole thing at `opacity` (0..255), inside the clip in force at begin_group(). Groups nest.
  // begin_group() returns false, with nothing started, when this renderer cannot (the default: no offscreen targets, too deep, out of memory,
  // or a layer's per-shape opacity is open); end_group() is called if and only if it returned true, and returns false if the picture could not be put on the target.
  virtual bool begin_group(const Rect2D& bounds) {
    (void)bounds;
    return false;
  }
  virtual bool end_group(const Matrix2D& m, uint8_t opacity, ImageFilter filter) {
    (void)m, (void)opacity, (void)filter;
    return false;
  }
  // Filters on a group's picture, done by the GPU. supports_group_filters() says whether this renderer has them; if so, begin_filter_group() is begin_group() for a
  // picture that is filtered: its bounds are not cut by the clip in force (a blur reads around what shows) and its own clip is its bounds alone; the clip applies when end_group()
  // lays the result over the target. Between the drawing and end_group(), group_blur() blurs the picture (a Gaussian of standard deviation `sigma` pixels, made of box blurs;
  // outside the picture is transparent) and group_color() applies a colour function; each returns false if it could not (the picture is then unfiltered at that step).
  virtual bool supports_group_filters() const { return false; }
  virtual bool begin_filter_group(const Rect2D& bounds) {
    (void)bounds;
    return false;
  }
  virtual bool group_blur(float sigma) {
    (void)sigma;
    return false;
  }
  virtual bool group_color(const ColorTransform& t) {
    (void)t;
    return false;
  }
  // Erases the coverage of a rounded rectangle (anti-aliased like fill_rounded_rect) from a filter group's picture: what was there is multiplied by one minus the coverage.
  virtual bool group_cut_out(const Rect2D& rect, const CornerRadii& radii) {
    (void)rect, (void)radii;
    return false;
  }

  // Fills `rect` with a gradient along the line from `from` to `to` (pixel coordinates; stops at 0 and 1 are
  // at those points, the ends extend as flat color). Stops must be sorted by offset; at most
  // kMaxGradientStops are used; no stops draws nothing; one stop is a flat fill.
  virtual void fill_linear_gradient(const Rect2D& rect, Vec2 from, Vec2 to, ArrayView<const GradientStop> stops) = 0;
  // Fills `rect` with an elliptical gradient: offset 1 is the ellipse around `center` with half axes
  // `radii` (both must be positive, else the last stop's color fills).
  virtual void fill_radial_gradient(const Rect2D& rect, Vec2 center, Vec2 radii, ArrayView<const GradientStop> stops) = 0;

  // Clip stack. The effective clip is set_clip()'s rectangle intersected with every pushed entry (set_clip
  // and reset_clip do not touch the pushed ones). Every push is matched by one pop_clip(), whatever it
  // returns. The return value is true when the clip is exact; false when it had to be approximated or could
  // not be honoured as asked (the push still happened): out of memory or stack depth, or - on the device
  // renderer - a rounded or mask clip, which only becomes its bounding rectangle there.
  virtual bool push_clip_rect(const Rect2D& rect) = 0;
  virtual bool push_clip_rounded_rect(const Rect2D& rect, const CornerRadii& radii) = 0;
  // Clip to the coverage of `mask` placed with its top left at (x, y); outside the mask nothing is drawn.
  virtual bool push_clip_mask(const MaskView& mask, int32_t x, int32_t y) = 0;
  virtual void pop_clip() = 0;

  // Group opacity: everything drawn until pop_layer() is composited onto what is below as one unit with
  // `opacity` (0..255), clipped to `bounds` and to the clip in force at the push. Layers nest; the clip
  // stack is private to a layer (pushes left open are dropped by pop_layer). True when exact; false when the
  // content could not be isolated (software: out of memory or depth, drawn straight onto the parent without
  // opacity; device: no offscreen targets, so each shape is drawn with the opacity itself, which differs
  // only where shapes overlap).
  virtual bool push_layer(const Rect2D& bounds, uint8_t opacity) = 0;
  virtual void pop_layer() = 0;

  // Scroll blit: copies the pixels of `source` so that its top left lands at (dest_x, dest_y) in the current
  // target, overlap safe. Limited by the clip rectangle (not by masks), ignores blend mode and opacity. The
  // device renderer has to read the frame back to do it (and returns false if that fails): use it sparingly there.
  // Returns true if the pixels were copied (also when the clip leaves nothing to copy).
  virtual bool copy_rect(const Rect2D& source, int32_t dest_x, int32_t dest_y) = 0;

  // Draws `source` of an image that lives on a graphics device, where it is, without the pixels passing through the CPU: false (and nothing drawn) unless this renderer draws on
  // that very device, the image can be sampled, or a layer's opacity is open. The image must be complete (no queued work) and stay valid until the call returns. The pixels are
  // composed like draw_image's (the current clip and blend mode).
  virtual bool draw_device_image(IGraphicsDevice& owner, IImage& image, const Rect2D& source, int32_t x, int32_t y) {
    (void)owner, (void)image, (void)source, (void)x, (void)y;
    return false;
  }

  // Makes everything drawn so far visible.
  virtual void present() = 0;

 protected:
  constexpr IRenderer2D() = default;
  ~IRenderer2D() = default;
};

}  // namespace graphics
