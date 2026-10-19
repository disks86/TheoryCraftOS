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
// Differences from the CPU renderer: lines are one-pixel-wide quads and circles
// are polygons, so their pixels are not identical; draw_image costs one rectangle
// per run of equal pixels (there are no textures yet), so keep images small.
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
  void present() override;

  // Draws what is batched, then copies the image to `out` (extent.width * extent.height, 0xAARRGGBB).
  bool read_pixels(ArrayView<uint32_t> out);

 private:
  static constexpr uint32_t kMaxVertices = 6 * 4096;
  static constexpr uint32_t kMaxRanges = 128;

  // A run of recorded vertices drawn with one clip and blend mode.
  struct Range {
    uint32_t first, count;
    Rect2D clip;
    BlendMode blend;
  };

  void triangle(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t color);
  void quad(float x0, float y0, float x1, float y1, uint32_t color);  // axis-aligned, corners
  void close_range();   // ends the run in progress (when the clip or blend mode changes)
  bool submit_pending();  // sends everything recorded to the device

  IGraphicsDevice& device_;
  allocators::IAllocator& allocator_;
  Extent2D extent_;
  IShaderModule* vs_ = nullptr;
  IShaderModule* fs_ = nullptr;
  IPipeline* blended_ = nullptr;
  IPipeline* opaque_ = nullptr;
  IBuffer* vertices_ = nullptr;
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
  Rect2D clip_;
  BlendMode blend_ = BlendMode::Alpha;
  bool valid_ = false;
};

}  // namespace graphics
