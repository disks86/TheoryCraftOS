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

#pragma once
#include "Allocators/IAllocator.hpp"
#include "Graphics/Graphics2D/IRenderer2D.hpp"
#include "Graphics/Graphics3D/BuiltinShaders.hpp"
#include "Graphics/Graphics3D/IGraphicsDevice.hpp"

namespace graphics {

// 2D drawing on top of any IGraphicsDevice: every shape becomes colored
// triangles (a rectangle is two, a circle a fan, text is one rectangle per run
// of set glyph pixels) collected in a vertex buffer and drawn in one batch with the
// built-in colored shaders. On the GPU device that is GPU-accelerated 2D; on the
// software device it works too, but SoftwareRenderer2D is the exact and faster
// choice there (SystemGraphics picks).
//
// Drawing only records: shapes are appended to the vertex buffer in runs that share a clip and a blend
// mode, and nothing reaches the device until present() (or read_pixels(), or when the buffer fills). That
// keeps the slow part - talking to a GPU that may take its time - out of the window's UI lock, which is held
// while the controls paint.
//
// The pixel-engine primitives are built from triangles too. Rounded rectangles, their borders and masks are
// pixel exact with the CPU renderer (solid parts are rectangles, curved edges are per-pixel coverage runs);
// gradients are triangles with per-vertex colors. Documented approximations (the push calls return false):
// rounded and mask clips scissor to their bounding rectangle, and layers have no offscreen target, so group
// opacity is applied to each shape (overlapping shapes show through each other). copy_rect reads the frame back.
//
// Differences from the CPU renderer: lines are one-pixel-wide quads and circles
// are polygons, so their pixels are not identical. draw_image of a large picture goes up as a texture and is one
// textured rectangle (the device samples it at pixel centres, so the pixels are the picture's); a small one, or one drawn
// under a layer's opacity, costs a rectangle per run of equal pixels.
class DeviceRenderer2D final : public IRenderer2D {
 public:
  DeviceRenderer2D(IGraphicsDevice& device, allocators::IAllocator& allocator, Extent2D extent);
  ~DeviceRenderer2D();
  DeviceRenderer2D(const DeviceRenderer2D&) = delete;
  DeviceRenderer2D& operator=(const DeviceRenderer2D&) = delete;

  // False if the device could not give us the resources (then nothing draws).
  bool valid() const { return valid_; }

  const char* name() const override { return "device 2D renderer"; }
  bool accelerated() const override { return device_.info().accelerated; }
  Extent2D extent() const override { return extent_; }

  void set_clip(const Rect2D& clip) override;
  void reset_clip() override;
  void set_blend(BlendMode mode) override;

  void clear(uint32_t color) override;
  void put_pixel(int32_t x, int32_t y, uint32_t color) override;
  void draw_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t color) override;
  void fill_rect(const Rect2D& rect, uint32_t color) override;
  void draw_rect(const Rect2D& rect, uint32_t color) override;
  void fill_circle(int32_t cx, int32_t cy, uint32_t radius, uint32_t color) override;
  void fill_triangle(Vec2 a, Vec2 b, Vec2 c, uint32_t color) override;
  void draw_image(const Surface& source, const Rect2D& source_rect, int32_t x, int32_t y) override;
  void draw_text(int32_t x, int32_t y, const char* text, uint32_t color) override;
  void fill_rounded_rect(const Rect2D& rect, const CornerRadii& radii, uint32_t color) override;
  void stroke_rounded_rect(const Rect2D& outer, const CornerRadii& radii, const EdgeWidths& widths, uint32_t color) override;
  void draw_mask(const MaskView& mask, const Rect2D& source_rect, int32_t x, int32_t y, uint32_t color) override;
  void draw_image_scaled(const Surface& source, const RectF& source_rect, const Rect2D& dest, ImageFilter filter) override;
  void fill_linear_gradient(const Rect2D& rect, Vec2 from, Vec2 to, ArrayView<const GradientStop> stops) override;
  void fill_radial_gradient(const Rect2D& rect, Vec2 center, Vec2 radii, ArrayView<const GradientStop> stops) override;
  bool push_clip_rect(const Rect2D& rect) override;
  bool push_clip_rounded_rect(const Rect2D& rect, const CornerRadii& radii) override;
  bool push_clip_mask(const MaskView& mask, int32_t x, int32_t y) override;
  void pop_clip() override;
  bool push_layer(const Rect2D& bounds, uint8_t opacity) override;
  void pop_layer() override;
  bool copy_rect(const Rect2D& source, int32_t dest_x, int32_t dest_y) override;
  bool draw_device_image(IGraphicsDevice& owner, IImage& image, const Rect2D& source, int32_t x, int32_t y) override;
  void present() override;

  // The color to draw for a pixel with `cov` (0..255) coverage; false: draw nothing (used by the rounded-rect helpers).
  bool coverage_color(uint32_t color, uint32_t cov, uint32_t* out) const;

  // Draws what is batched, then copies the image to `out` (extent.width * extent.height, 0xAARRGGBB).
  bool read_pixels(ArrayView<uint32_t> out);

 private:
  static constexpr uint32_t kMaxVertices = 6 * 4096;
  static constexpr uint32_t kMaxRanges = 512;
  // The sheet: small masks (glyphs) of a frame are baked, colored, into one texture and drawn as textured rectangles in the batch, in order with the other shapes.
  static constexpr uint32_t kSheetSize = 512, kSheetMaxMask = 128, kMaxSheetVertices = 6 * 4096;

  // A run of recorded vertices drawn with one clip and blend mode.
  struct Range {
    uint32_t first, count;
    Rect2D clip;
    BlendMode blend;
    bool sheet;   // the vertices are the sheet's (textured), not the colored batch's
  };

  void triangle_colors(const float* xs, const float* ys, const Vec4* colors);  // three vertices, own colors
  void apply_clip();     // clip_ from base_clip_ and the pushed rectangles
  void triangle(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t color);
  void quad(float x0, float y0, float x1, float y1, uint32_t color);  // axis-aligned, corners
  void close_range();   // ends the run in progress (when the clip or blend mode changes)
  bool submit_pending();  // sends everything recorded to the device
  bool draw_texture(const Surface& source, const Rect2D& rect, int32_t x, int32_t y);  // `rect` of the picture as one textured rectangle; false: not done
  bool ensure_texture(uint32_t width, uint32_t height);
  bool upload_and_draw(uint64_t w, uint64_t h, int64_t dx, int64_t dy);   // texture_rows_ (w x h, 0xAARRGGBB) as one rectangle at (dx, dy)
  bool prepare_rows(uint64_t count);   // texture_rows_ big enough for `count` pixels
  bool draw_mask_textured(const MaskView& mask, int64_t ddx, int64_t ddy, int64_t dx0, int64_t dy0, uint64_t w, uint64_t h, uint32_t color);
  bool sheet_slot(uint32_t w, uint32_t h, uint32_t* x, uint32_t* y);   // room for a w x h mask on the sheet (a submit and a fresh sheet if it is full)
  void sheet_emit(uint32_t sx, uint32_t sy, uint32_t w, uint32_t h, int64_t dx, int64_t dy);   // the rectangle that shows it
  void close_sheet_range();
  void reset_sheet();
  bool draw_textured_quad(IImage& image, int64_t dx, int64_t dy, uint32_t w, uint32_t h, float u0, float v0, float u1, float v1);

  IGraphicsDevice& device_;
  allocators::IAllocator& allocator_;
  Extent2D extent_;
  IShaderModule* vs_ = nullptr;
  IShaderModule* fs_ = nullptr;
  IPipeline* blended_ = nullptr;
  IPipeline* opaque_ = nullptr;
  IBuffer* vertices_ = nullptr;
  // Pictures as textures: the textured pair of pipelines, six vertices for the rectangle, the texture (kept while pictures stay the same size) and the rows it is filled from.
  IShaderModule* texture_vs_ = nullptr;
  IShaderModule* texture_fs_ = nullptr;
  IPipeline* texture_blended_ = nullptr;
  IPipeline* texture_opaque_ = nullptr;
  IBuffer* texture_vertices_ = nullptr;
  IImage* texture_ = nullptr;
  Extent2D texture_extent_;
  ArrayView<uint32_t> texture_rows_;
  IBuffer* sheet_vertices_ = nullptr;
  IImage* sheet_ = nullptr;
  ArrayView<uint32_t> sheet_pixels_;        // the sheet's contents (kSheetSize x kSheetSize), written to sheet_ when a submit needs them
  ArrayView<TexturedVertex> sheet_batch_;   // the mapped vertex buffer
  uint32_t sheet_count_ = 0, sheet_range_start_ = 0;
  uint32_t sheet_x_ = 0, sheet_y_ = 0, sheet_shelf_ = 0;   // where the next mask goes: the shelf in use
  bool sheet_dirty_ = false;                // pixels added since the last upload
  bool sheet_ok_ = false;                   // all of it exists
  // Double buffered: frames are drawn into images_[back_] and present() shows that image and then draws into the other, so the screen
  // never shows a half-drawn frame. (Every frame must repaint all of the target: the back image holds the frame before last.)
  IImage* images_[2] = {};
  uint32_t back_ = 0;
  ICommandBuffer* commands_ = nullptr;
  ArrayView<ColoredVertex> batch_;  // the mapped vertex buffer
  uint32_t count_ = 0;         // vertices recorded
  uint32_t range_start_ = 0;   // where the run in progress began
  Range ranges_[kMaxRanges];
  uint32_t range_count_ = 0;
  Rect2D clip_;       // effective scissor
  Rect2D base_clip_;  // set_clip()
  Rect2D clip_stack_[kMaxClipDepth];
  uint32_t clip_depth_ = 0, clip_overflow_ = 0;
  float opacity_ = 1.0f;  // product of the open layers' opacities, applied to every vertex color
  float layer_opacity_[kMaxLayerDepth];
  uint32_t layer_clip_depth_[kMaxLayerDepth] = {};  // clip stack depth at each push_layer (pop_layer drops what was pushed since)
  uint32_t layer_clip_overflow_[kMaxLayerDepth] = {};
  uint32_t layer_depth_ = 0, layer_overflow_ = 0;
  BlendMode blend_ = BlendMode::Alpha;
  bool valid_ = false;
};

}  // namespace graphics
