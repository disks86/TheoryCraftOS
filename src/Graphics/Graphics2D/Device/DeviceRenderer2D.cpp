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

#include "Graphics/Graphics2D/Device/DeviceRenderer2D.hpp"
#include "Graphics/Graphics2D/Raster2D.hpp"
#include "Hardware/GraphicsProcessingUnit/Font8x16.hpp"
#include "Utility/AllocatedView.hpp"

namespace graphics {

DeviceRenderer2D::DeviceRenderer2D(IGraphicsDevice& device, allocators::IAllocator& allocator, Extent2D extent)
    : device_(device), allocator_(allocator), extent_(extent) {
  clip_ = base_clip_ = {0, 0, extent.width, extent.height};
  vs_ = device.create_shader_module(colored_vertex_shader());
  fs_ = device.create_shader_module(colored_fragment_shader());
  GraphicsPipelineDesc desc;
  describe_colored_pipeline(&desc);
  desc.vertex_shader = vs_;
  desc.fragment_shader = fs_;
  desc.cull_mode = CullMode::None;
  if (vs_ && fs_) {
    desc.alpha_blend = true;
    blended_ = device.create_graphics_pipeline(desc);
    desc.alpha_blend = false;
    opaque_ = device.create_graphics_pipeline(desc);
  }
  vertices_ = device.create_buffer(sizeof(ColoredVertex) * kMaxVertices, kBufferVertex);
  // (best effort: a device without textures draws pictures as runs of rectangles)
  texture_vs_ = device.create_shader_module(textured_vertex_shader());
  texture_fs_ = device.create_shader_module(textured_fragment_shader());
  if (texture_vs_ && texture_fs_) {
    GraphicsPipelineDesc textured;
    describe_textured_pipeline(&textured);
    textured.vertex_shader = texture_vs_;
    textured.fragment_shader = texture_fs_;
    textured.cull_mode = CullMode::None;
    textured.alpha_blend = true;
    texture_blended_ = device.create_graphics_pipeline(textured);
    textured.alpha_blend = false;
    texture_opaque_ = device.create_graphics_pipeline(textured);
    texture_vertices_ = device.create_buffer(sizeof(TexturedVertex) * 6, kBufferVertex);
  }
  for (IImage*& image : images_) image = device.create_image({extent, Format::B8G8R8A8Unorm, kImageColorAttachment | kImagePresent});
  commands_ = device.create_command_buffer();
  valid_ = blended_ && opaque_ && vertices_ && images_[0] && images_[1] && commands_;
  if (valid_) {
    batch_ = vertices_->map().cast<ColoredVertex>();
    valid_ = !batch_.empty();
  }
}

DeviceRenderer2D::~DeviceRenderer2D() {
  if (texture_) device_.destroy_image(texture_);
  release_view(allocator_, texture_rows_);
  if (texture_vertices_) device_.destroy_buffer(texture_vertices_);
  if (texture_opaque_) device_.destroy_pipeline(texture_opaque_);
  if (texture_blended_) device_.destroy_pipeline(texture_blended_);
  if (texture_fs_) device_.destroy_shader_module(texture_fs_);
  if (texture_vs_) device_.destroy_shader_module(texture_vs_);
  if (commands_) device_.destroy_command_buffer(commands_);
  for (IImage* image : images_)
    if (image) device_.destroy_image(image);
  if (vertices_) device_.destroy_buffer(vertices_);
  if (opaque_) device_.destroy_pipeline(opaque_);
  if (blended_) device_.destroy_pipeline(blended_);
  if (fs_) device_.destroy_shader_module(fs_);
  if (vs_) device_.destroy_shader_module(vs_);
}

void DeviceRenderer2D::close_range() {
  if (count_ <= range_start_) return;
  ranges_[range_count_++] = {range_start_, count_ - range_start_, clip_, blend_};
  range_start_ = count_;
  if (range_count_ == kMaxRanges) submit_pending();
}

bool DeviceRenderer2D::submit_pending() {
  close_range();
  if (!valid_ || range_count_ == 0) return true;
  vertices_->unmap();
  commands_->begin();
  RenderPassBegin pass;
  pass.color = images_[back_];
  pass.clear_color = false;  // keep what earlier frames and submissions drew
  commands_->begin_render_pass(pass);
  commands_->bind_vertex_buffer(vertices_);
  // Pixel coordinates (0,0 top left) to clip space; depth is constant.
  ColoredConstants constants;
  constants.mvp.m[0] = 2.0f / extent_.width;
  constants.mvp.m[5] = 2.0f / extent_.height;
  constants.mvp.m[10] = 0;
  constants.mvp.m[12] = -1;
  constants.mvp.m[13] = -1;
  constants.mvp.m[14] = 0.5f;
  bool pushed = false;
  for (uint32_t i = 0; i < range_count_; ++i) {
    const Range& r = ranges_[i];
    commands_->set_scissor(r.clip);
    commands_->bind_pipeline(r.blend == BlendMode::Alpha ? blended_ : opaque_);
    if (!pushed) {  // (constants stay with the commands; binding another pipeline does not lose them)
      commands_->push_constants(&constants, sizeof(constants));
      pushed = true;
    }
    commands_->draw(r.count, r.first);
  }
  commands_->end_render_pass();
  const bool ok = commands_->end() && device_.submit(commands_);
  count_ = range_start_ = range_count_ = 0;
  return ok;
}

void DeviceRenderer2D::triangle(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t color) {
  const float xs[3] = {x0, x1, x2}, ys[3] = {y0, y1, y2};
  const Vec4 c = unpack_color(color);
  const Vec4 cs[3] = {c, c, c};
  triangle_colors(xs, ys, cs);
}

void DeviceRenderer2D::triangle_colors(const float* xs, const float* ys, const Vec4* colors) {
  if (!valid_ || clip_.width == 0 || clip_.height == 0) return;  // (nothing can show through an empty scissor)
  if (count_ + 3 > kMaxVertices) submit_pending();
  for (int i = 0; i < 3; ++i) batch_[count_++] = {{xs[i], ys[i], 0}, {colors[i].x, colors[i].y, colors[i].z, colors[i].w * opacity_}};
}

void DeviceRenderer2D::quad(float x0, float y0, float x1, float y1, uint32_t color) {
  triangle(x0, y0, x1, y0, x1, y1, color);
  triangle(x0, y0, x1, y1, x0, y1, color);
}

void DeviceRenderer2D::apply_clip() {
  close_range();  // what is recorded belongs to the old clip
  Rect2D r = base_clip_;
  for (uint32_t i = 0; i < clip_depth_; ++i) r = raster::intersect(r, clip_stack_[i]);
  if (clip_overflow_) r.width = r.height = 0;  // fail closed
  clip_ = r;
}

void DeviceRenderer2D::set_clip(const Rect2D& clip) {
  base_clip_ = raster::intersect(clip, {0, 0, extent_.width, extent_.height});
  apply_clip();
}

void DeviceRenderer2D::reset_clip() {
  base_clip_ = {0, 0, extent_.width, extent_.height};
  apply_clip();
}

void DeviceRenderer2D::set_blend(BlendMode mode) {
  if (mode != blend_) close_range();
  blend_ = mode;
}

void DeviceRenderer2D::clear(uint32_t color) {
  if (!valid_) return;
  count_ = range_start_ = range_count_ = 0;  // the clear wipes whatever was recorded
  commands_->begin();
  RenderPassBegin pass;
  pass.color = images_[back_];
  pass.clear_color_value = unpack_color(color);
  commands_->begin_render_pass(pass);
  commands_->end_render_pass();
  if (commands_->end()) device_.submit(commands_);
}

void DeviceRenderer2D::put_pixel(int32_t x, int32_t y, uint32_t color) {
  quad(static_cast<float>(x), static_cast<float>(y), static_cast<float>(x) + 1, static_cast<float>(y) + 1, color);
}

void DeviceRenderer2D::draw_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t color) {
  const float ax = x0 + 0.5f, ay = y0 + 0.5f, bx = x1 + 0.5f, by = y1 + 0.5f;
  const float dx = bx - ax, dy = by - ay;
  const float length = static_cast<float>(fmath::sqrt(dx * dx + dy * dy));
  if (length < 1e-4f) {
    put_pixel(x0, y0, color);
    return;
  }
  const float nx = -dy / length * 0.5f, ny = dx / length * 0.5f;  // half a pixel to each side
  triangle(ax + nx, ay + ny, ax - nx, ay - ny, bx + nx, by + ny, color);
  triangle(ax - nx, ay - ny, bx - nx, by - ny, bx + nx, by + ny, color);
}

void DeviceRenderer2D::fill_rect(const Rect2D& r, uint32_t color) {
  if (!r.width || !r.height) return;
  quad(static_cast<float>(r.x), static_cast<float>(r.y), static_cast<float>(r.x) + static_cast<float>(r.width),
       static_cast<float>(r.y) + static_cast<float>(r.height), color);
}

void DeviceRenderer2D::draw_rect(const Rect2D& r, uint32_t color) {
  if (!r.width || !r.height) return;
  const float x0 = static_cast<float>(r.x), y0 = static_cast<float>(r.y);
  const float x1 = x0 + static_cast<float>(r.width), y1 = y0 + static_cast<float>(r.height);
  quad(x0, y0, x1, y0 + 1, color);          // top
  if (r.height > 1) quad(x0, y1 - 1, x1, y1, color);  // bottom
  if (r.height > 2) {
    quad(x0, y0 + 1, x0 + 1, y1 - 1, color);  // left
    if (r.width > 1) quad(x1 - 1, y0 + 1, x1, y1 - 1, color);  // right
  }
}

void DeviceRenderer2D::fill_circle(int32_t cx, int32_t cy, uint32_t radius, uint32_t color) {
  // The CPU renderer covers pixels whose centers lie within `radius` of the middle of pixel (cx, cy).
  const float r = static_cast<float>(radius) + 0.5f, px = cx + 0.5f, py = cy + 0.5f;
  uint32_t segments = 12 + radius * 2;
  if (segments > 96) segments = 96;
  float prev_x = px + r, prev_y = py;
  for (uint32_t i = 1; i <= segments; ++i) {
    const double angle = 6.28318530717958647692 * i / segments;
    const float x = px + r * static_cast<float>(fmath::cos(angle)), y = py + r * static_cast<float>(fmath::sin(angle));
    triangle(px, py, prev_x, prev_y, x, y, color);
    prev_x = x;
    prev_y = y;
  }
}

void DeviceRenderer2D::fill_triangle(Vec2 a, Vec2 b, Vec2 c, uint32_t color) {
  triangle(a.x, a.y, b.x, b.y, c.x, c.y, color);
}

// Pictures smaller than this (in pixels) stay runs of rectangles: sending them as a texture breaks the batch and costs a submit.
static constexpr uint64_t kTextureMinPixels = 64 * 64;

bool DeviceRenderer2D::ensure_texture(uint32_t width, uint32_t height) {
  if (texture_ && texture_extent_.width == width && texture_extent_.height == height) return true;
  if (texture_) device_.destroy_image(texture_);
  texture_ = device_.create_image({{width, height}, Format::B8G8R8A8Unorm, kImageSampled});
  texture_extent_ = {width, height};
  return texture_ != nullptr;
}

bool DeviceRenderer2D::draw_texture(const Surface& s, const Rect2D& rect, int32_t x, int32_t y) {
  if (!texture_blended_ || !texture_opaque_ || !texture_vertices_ || opacity_ != 1.0f) return false;
  // The part of the picture that exists (what is outside it draws nothing).
  const int64_t sx0 = rect.x > 0 ? rect.x : 0, sy0 = rect.y > 0 ? rect.y : 0;
  const int64_t sx1 = int64_t{rect.x} + rect.width < s.width ? int64_t{rect.x} + rect.width : s.width;
  const int64_t sy1 = int64_t{rect.y} + rect.height < s.height ? int64_t{rect.y} + rect.height : s.height;
  if (sx0 >= sx1 || sy0 >= sy1) return true;
  const uint32_t w = static_cast<uint32_t>(sx1 - sx0), h = static_cast<uint32_t>(sy1 - sy0);
  const uint64_t count = uint64_t{w} * h;
  if (count < kTextureMinPixels) return false;
  const int64_t dx = int64_t{x} + (sx0 - rect.x), dy = int64_t{y} + (sy0 - rect.y);
  const Rect2D area = raster::intersect({raster::saturate_i32(dx), raster::saturate_i32(dy), w, h}, clip_);
  if (!area.width || !area.height) return true;   // (nothing of it is inside the clip)
  if (texture_rows_.size() < count) {
    release_view(allocator_, texture_rows_);
    texture_rows_ = allocate_view<uint32_t>(allocator_, count);
    if (texture_rows_.empty()) return false;
  }
  const ArrayView<uint32_t> rows = texture_rows_.first(count);
  for (uint32_t j = 0; j < h; ++j) {
    const ArrayView<const uint32_t> from = ArrayView<const uint32_t>(s.row(static_cast<uint32_t>(sy0) + j), s.width).subview(static_cast<uint64_t>(sx0), w);
    const ArrayView<uint32_t> to = rows.subview(uint64_t{j} * w, w);
    if (s.format == Format::B8G8R8A8Unorm) to.copy_from(from);
    else
      for (uint32_t i = 0; i < w; ++i) to[i] = s.decode(from[i]);
  }
  if (!submit_pending() || !ensure_texture(w, h) || !texture_->write_pixels(ArrayView<const uint32_t>(rows.data(), rows.size()))) return false;

  return draw_textured_quad(*texture_, dx, dy, w, h, 0.0f, 0.0f, 1.0f, 1.0f);
}

// One rectangle at (dx, dy), w x h pixels, showing the part (u0, v0)-(u1, v1) of the image: drawn at once, in a submit of its own after what was recorded before it.
bool DeviceRenderer2D::draw_textured_quad(IImage& image, int64_t dx, int64_t dy, uint32_t w, uint32_t h, float u0, float v0, float u1, float v1) {
  if (!submit_pending()) return false;
  const float x0 = static_cast<float>(dx), y0 = static_cast<float>(dy), x1 = x0 + static_cast<float>(w), y1 = y0 + static_cast<float>(h);
  const TexturedVertex quad[6] = {{{x0, y0, 0}, {u0, v0}}, {{x1, y0, 0}, {u1, v0}}, {{x1, y1, 0}, {u1, v1}}, {{x0, y0, 0}, {u0, v0}}, {{x1, y1, 0}, {u1, v1}}, {{x0, y1, 0}, {u0, v1}}};
  const ArrayView<TexturedVertex> mapped = texture_vertices_->map().cast<TexturedVertex>();
  if (mapped.size() < 6) return false;
  mapped.copy_from(ArrayView<const TexturedVertex>(quad, 6));
  texture_vertices_->unmap();

  commands_->begin();
  RenderPassBegin pass;
  pass.color = images_[back_];
  pass.clear_color = false;
  commands_->begin_render_pass(pass);
  commands_->bind_pipeline(blend_ == BlendMode::Alpha ? texture_blended_ : texture_opaque_);
  commands_->bind_vertex_buffer(texture_vertices_);
  SamplerDesc sampler;   // (nearest at the pixel centres: the picture's own pixels)
  sampler.address_u = sampler.address_v = AddressMode::ClampToEdge;
  commands_->bind_texture(0, &image, sampler);
  ColoredConstants constants;
  constants.mvp.m[0] = 2.0f / extent_.width;
  constants.mvp.m[5] = 2.0f / extent_.height;
  constants.mvp.m[10] = 0;
  constants.mvp.m[12] = -1;
  constants.mvp.m[13] = -1;
  constants.mvp.m[14] = 0.5f;
  commands_->push_constants(&constants, sizeof(constants));
  commands_->set_scissor(clip_);
  commands_->draw(6);
  commands_->end_render_pass();
  return commands_->end() && device_.submit(commands_);
}

bool DeviceRenderer2D::draw_device_image(IGraphicsDevice& owner, IImage& image, const Rect2D& rect, int32_t x, int32_t y) {
  if (!valid_ || &owner != &device_ || !texture_blended_ || !texture_opaque_ || !texture_vertices_ || opacity_ != 1.0f) return false;
  const Extent2D e = image.extent();
  const int64_t sx0 = rect.x > 0 ? rect.x : 0, sy0 = rect.y > 0 ? rect.y : 0;
  const int64_t sx1 = int64_t{rect.x} + rect.width < e.width ? int64_t{rect.x} + rect.width : e.width;
  const int64_t sy1 = int64_t{rect.y} + rect.height < e.height ? int64_t{rect.y} + rect.height : e.height;
  if (sx0 >= sx1 || sy0 >= sy1) return true;
  const uint32_t w = static_cast<uint32_t>(sx1 - sx0), h = static_cast<uint32_t>(sy1 - sy0);
  const int64_t dx = int64_t{x} + (sx0 - rect.x), dy = int64_t{y} + (sy0 - rect.y);
  if (raster::intersect({raster::saturate_i32(dx), raster::saturate_i32(dy), w, h}, clip_).width == 0) return true;
  const float iw = static_cast<float>(e.width), ih = static_cast<float>(e.height);
  return draw_textured_quad(image, dx, dy, w, h, static_cast<float>(sx0) / iw, static_cast<float>(sy0) / ih, static_cast<float>(sx1) / iw, static_cast<float>(sy1) / ih);
}

void DeviceRenderer2D::draw_image(const Surface& s, const Rect2D& rect, int32_t x, int32_t y) {
  if (!s.valid()) return;
  if (valid_ && draw_texture(s, rect, x, y)) return;
  for (uint32_t j = 0; j < rect.height; ++j) {
    const int64_t sy = static_cast<int64_t>(rect.y) + j;
    if (sy < 0 || sy >= s.height) continue;
    uint32_t i = 0;
    while (i < rect.width) {
      const int64_t sx = static_cast<int64_t>(rect.x) + i;
      if (sx < 0 || sx >= s.width) {
        ++i;
        continue;
      }
      const uint32_t pixel = s.decode(s.row(static_cast<uint32_t>(sy))[sx]);
      uint32_t run = 1;  // extend over equal neighbours
      while (i + run < rect.width && rect.x + static_cast<int64_t>(i + run) < s.width &&
             s.decode(s.row(static_cast<uint32_t>(sy))[rect.x + i + run]) == pixel)
        ++run;
      fill_rect({raster::saturate_i32(int64_t{x} + i), raster::saturate_i32(int64_t{y} + j), run, 1}, pixel);
      i += run;
    }
  }
}

void DeviceRenderer2D::draw_text(int32_t x, int32_t y, const char* text, uint32_t color) {
  for (; *text; ++text, x += static_cast<int32_t>(hardware::Font8x16::kWidth)) {
    const ConstByteView glyph = hardware::Font8x16::glyph(*text);
    for (uint32_t j = 0; j < hardware::Font8x16::kHeight; ++j) {
      uint32_t i = 0;
      while (i < hardware::Font8x16::kWidth) {
        if (!(glyph[j] & (0x80 >> i))) {
          ++i;
          continue;
        }
        uint32_t run = 1;
        while (i + run < hardware::Font8x16::kWidth && (glyph[j] & (0x80 >> (i + run)))) ++run;
        fill_rect({x + static_cast<int32_t>(i), y + static_cast<int32_t>(j), run, 1}, color);
        i += run;
      }
    }
  }
}

// ---- Pixel-engine primitives -----------------------------------------------------------------------------

bool DeviceRenderer2D::coverage_color(uint32_t color, uint32_t cov, uint32_t* out) const {
  if (blend_ == BlendMode::Replace) {  // no anti-aliasing: half coverage or more is a whole pixel
    if (cov < 128) return false;
    *out = color;
    return true;
  }
  if (cov == 0) return false;
  *out = cov == 255 ? color : raster::scale_alpha(color, cov);
  return true;
}

namespace {
// Collects what raster::rrect_rows reports into as few rectangles as possible: equal solid runs on adjacent
// rows become one rectangle, equal neighbouring partial pixels one run.
struct RectSink {
  DeviceRenderer2D& r;
  uint32_t color;
  Rect2D clip;
  bool has_rect = false, has_run = false;
  int32_t rx0 = 0, rx1 = 0, ry0 = 0, ry1 = 0;
  int32_t px0 = 0, px1 = 0, py = 0;
  uint32_t pcolor = 0, pcov = 0;

  void flush_rect() {
    if (has_rect) r.fill_rect({rx0, ry0, static_cast<uint32_t>(rx1 - rx0), static_cast<uint32_t>(ry1 - ry0)}, color);
    has_rect = false;
  }
  void flush_run() {
    if (has_run) r.fill_rect({px0, py, static_cast<uint32_t>(px1 - px0), 1}, pcolor);
    has_run = false;
  }
  void solid(int32_t y, int32_t xa, int32_t xb) {
    xa = raster::max_i(xa, clip.x);
    xb = raster::min_i(xb, raster::saturate_i32(int64_t{clip.x} + clip.width));
    if (xa >= xb) return;
    if (has_rect && xa == rx0 && xb == rx1 && y == ry1) {
      ++ry1;
      return;
    }
    flush_rect();
    has_rect = true;
    rx0 = xa, rx1 = xb, ry0 = y, ry1 = y + 1;
  }
  void pixel(int32_t x, int32_t y, uint32_t cov) {
    if (x < clip.x || x >= raster::saturate_i32(int64_t{clip.x} + clip.width)) return;
    if (has_run && y == py && x == px1 && cov == pcov) {
      ++px1;
      return;
    }
    flush_run();
    uint32_t c;
    if (!r.coverage_color(color, cov, &c)) return;
    has_run = true;
    px0 = x, px1 = x + 1, py = y, pcolor = c, pcov = cov;
  }
  void finish() {
    flush_rect();
    flush_run();
  }
};
}  // namespace

void DeviceRenderer2D::fill_rounded_rect(const Rect2D& rect, const CornerRadii& radii, uint32_t color) {
  if (!valid_ || !rect.width || !rect.height) return;
  const raster::RRect rr = raster::RRect::make(rect, radii);
  RectSink sink{*this, color, clip_};
  raster::rrect_rows(rr, nullptr, clip_.y, raster::saturate_i32(int64_t{clip_.y} + clip_.height), sink);
  sink.finish();
}

void DeviceRenderer2D::stroke_rounded_rect(const Rect2D& outer, const CornerRadii& radii, const EdgeWidths& widths, uint32_t color) {
  if (!valid_ || !outer.width || !outer.height) return;
  const raster::RRect rr = raster::RRect::make(outer, radii);
  const raster::RRect hole = raster::RRect::inset(rr, widths);
  RectSink sink{*this, color, clip_};
  raster::rrect_rows(rr, &hole, clip_.y, raster::saturate_i32(int64_t{clip_.y} + clip_.height), sink);
  sink.finish();
}

void DeviceRenderer2D::draw_mask(const MaskView& mask, const Rect2D& rect, int32_t x, int32_t y, uint32_t color) {
  if (!valid_ || !mask.valid()) return;
  const int64_t sx0 = rect.x > 0 ? rect.x : 0, sy0 = rect.y > 0 ? rect.y : 0;
  const int64_t sx1 = raster::min_i(raster::saturate_i32(int64_t{rect.x} + rect.width), raster::saturate_i32(mask.width));
  const int64_t sy1 = raster::min_i(raster::saturate_i32(int64_t{rect.y} + rect.height), raster::saturate_i32(mask.height));
  const int64_t ddx = int64_t{x} - rect.x, ddy = int64_t{y} - rect.y;
  const int64_t dx0 = raster::max_i(raster::saturate_i32(sx0 + ddx), clip_.x), dx1 = raster::min_i(raster::saturate_i32(sx1 + ddx), raster::saturate_i32(int64_t{clip_.x} + clip_.width));
  const int64_t dy0 = raster::max_i(raster::saturate_i32(sy0 + ddy), clip_.y), dy1 = raster::min_i(raster::saturate_i32(sy1 + ddy), raster::saturate_i32(int64_t{clip_.y} + clip_.height));
  if (dx0 >= dx1 || dy0 >= dy1) return;
  const uint64_t n = static_cast<uint64_t>(dx1 - dx0);
  for (int64_t dy = dy0; dy < dy1; ++dy) {
    const ConstByteView cov = mask.coverage.subview(static_cast<uint64_t>(dy - ddy) * mask.pitch + static_cast<uint64_t>(dx0 - ddx), n);
    for (uint64_t i = 0; i < n;) {
      uint64_t j = i + 1;
      while (j < n && cov[j] == cov[i]) ++j;  // a run of equal coverage is one rectangle
      uint32_t c;
      if (coverage_color(color, cov[i], &c))
        fill_rect({static_cast<int32_t>(dx0 + static_cast<int64_t>(i)), static_cast<int32_t>(dy), static_cast<uint32_t>(j - i), 1}, c);
      i = j;
    }
  }
}

void DeviceRenderer2D::draw_image_scaled(const Surface& source, const RectF& src, const Rect2D& dest, ImageFilter filter) {
  if (!valid_ || !dest.width || !dest.height) return;
  const raster::SampleBounds bounds = raster::sample_bounds(source, src);
  if (bounds.empty()) return;
  const Rect2D area = raster::intersect(dest, clip_);
  if (!area.width || !area.height) return;
  const float step_x = src.width / static_cast<float>(dest.width), step_y = src.height / static_cast<float>(dest.height);
  const int64_t col0 = int64_t{area.x} - dest.x, row0 = int64_t{area.y} - dest.y;
  for (uint32_t j = 0; j < area.height; ++j) {
    const float sy = src.y + (static_cast<float>(row0 + j) + 0.5f) * step_y;
    uint32_t run_start = 0, run_color = 0;
    for (uint32_t i = 0; i <= area.width; ++i) {
      uint32_t argb = 0;
      if (i < area.width) {
        const float sx = src.x + (static_cast<float>(col0 + i) + 0.5f) * step_x;
        argb = filter == ImageFilter::Nearest ? raster::sample_nearest(source, bounds, sx, sy) : raster::sample_bilinear(source, bounds, sx, sy);
      }
      if (i > 0 && (i == area.width || argb != run_color))  // end of a run of equal pixels
        fill_rect({area.x + static_cast<int32_t>(run_start), area.y + static_cast<int32_t>(j), i - run_start, 1}, run_color);
      if (i == 0 || argb != run_color) run_start = i, run_color = argb;
    }
  }
}

namespace {
struct Pt {
  float x, y;
};
// Keeps the part of the convex polygon `in` where plane(p) = sign * (a * x + b * y + c) >= 0.
uint32_t clip_polygon(const Pt* in, uint32_t n, float a, float b, float c, Pt* out) {
  uint32_t m = 0;
  for (uint32_t i = 0; i < n; ++i) {
    const Pt p = in[i], q = in[(i + 1) % n];
    const float dp = a * p.x + b * p.y + c, dq = a * q.x + b * q.y + c;
    if (dp >= 0) out[m++] = p;
    if ((dp >= 0) != (dq >= 0)) {
      const float k = dp / (dp - dq);
      out[m++] = {p.x + (q.x - p.x) * k, p.y + (q.y - p.y) * k};
    }
  }
  return m;
}

// Breakpoints of a gradient in (lo, hi): every stop offset in range, plus subdivisions of segments whose alpha
// changes (the GPU would interpolate straight alpha; the CPU does premultiplied, which differs only there).
uint32_t gradient_breakpoints(const raster::GradientPrep& g, float lo, float hi, float* out) {
  uint32_t n = 0;
  out[n++] = lo;
  for (uint32_t i = 0; i < g.count; ++i) {
    if (g.offset[i] > lo && g.offset[i] < hi) out[n++] = g.offset[i];
    if (i + 1 < g.count && (g.color[i] >> 24) != (g.color[i + 1] >> 24)) {
      for (uint32_t k = 1; k < 8; ++k) {
        const float t = g.offset[i] + (g.offset[i + 1] - g.offset[i]) * (static_cast<float>(k) / 8.0f);
        if (t > lo && t < hi) out[n++] = t;
      }
    }
  }
  out[n++] = hi;
  return n;
}
constexpr uint32_t kMaxBreakpoints = kMaxGradientStops * 8 + 2;
}  // namespace

void DeviceRenderer2D::fill_linear_gradient(const Rect2D& rect, Vec2 from, Vec2 to, ArrayView<const GradientStop> stops) {
  raster::GradientPrep g;
  if (!valid_ || !g.build(stops)) return;
  const Rect2D area = raster::intersect(rect, clip_);
  if (!area.width || !area.height) return;
  const float dx = to.x - from.x, dy = to.y - from.y, len2 = dx * dx + dy * dy;
  if (!(len2 > 1e-12f) || g.count == 1) {
    fill_rect(area, g.color[g.count - 1]);
    return;
  }
  const float inv = 1.0f / len2, tx = dx * inv, ty = dy * inv;
  const float x0 = static_cast<float>(area.x), y0 = static_cast<float>(area.y);
  const float x1 = x0 + static_cast<float>(area.width), y1 = y0 + static_cast<float>(area.height);
  const Pt corners[4] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
  float tmin = 1e30f, tmax = -1e30f;
  for (const Pt& p : corners) {
    const float t = (p.x - from.x) * tx + (p.y - from.y) * ty;
    tmin = t < tmin ? t : tmin;
    tmax = t > tmax ? t : tmax;
  }
  float bp[kMaxBreakpoints];
  const uint32_t n = gradient_breakpoints(g, tmin, tmax, bp);
  const float c0 = -(from.x * tx + from.y * ty);  // t(p) = tx * x + ty * y + c0
  for (uint32_t i = 0; i + 1 < n; ++i) {
    const float lo = bp[i], hi = bp[i + 1];
    if (!(hi > lo)) continue;
    const uint32_t seg = g.segment_of((lo + hi) * 0.5f);
    Pt a[8], b[8];
    uint32_t m = clip_polygon(corners, 4, tx, ty, c0 - lo, a);   // t >= lo
    m = clip_polygon(a, m, -tx, -ty, hi - c0, b);                // t <= hi
    if (m < 3) continue;
    Vec4 col[8];
    for (uint32_t k = 0; k < m; ++k) {
      const float t = raster::clampf(tx * b[k].x + ty * b[k].y + c0, lo, hi);
      col[k] = unpack_color(g.color_in_segment(seg, t));
    }
    for (uint32_t k = 1; k + 1 < m; ++k) {
      const float xs[3] = {b[0].x, b[k].x, b[k + 1].x}, ys[3] = {b[0].y, b[k].y, b[k + 1].y};
      const Vec4 cs[3] = {col[0], col[k], col[k + 1]};
      triangle_colors(xs, ys, cs);
    }
  }
}

void DeviceRenderer2D::fill_radial_gradient(const Rect2D& rect, Vec2 center, Vec2 radii, ArrayView<const GradientStop> stops) {
  raster::GradientPrep g;
  if (!valid_ || !g.build(stops)) return;
  const Rect2D area = raster::intersect(rect, clip_);
  if (!area.width || !area.height) return;
  if (!(radii.x > 0) || !(radii.y > 0) || g.count == 1) {
    fill_rect(area, g.color[g.count - 1]);
    return;
  }
  const float x0 = static_cast<float>(area.x), y0 = static_cast<float>(area.y);
  const float xs_[2] = {x0, x0 + static_cast<float>(area.width)}, ys_[2] = {y0, y0 + static_cast<float>(area.height)};
  float tmax = 0;
  for (float cx : xs_)
    for (float cy : ys_) {
      const float u = (cx - center.x) / radii.x, v = (cy - center.y) / radii.y;
      const float t = raster::sqrt_f(u * u + v * v);
      tmax = t > tmax ? t : tmax;
    }
  if (!(tmax > 0)) {
    fill_rect(area, g.color[0]);
    return;
  }
  float bp[kMaxBreakpoints];
  const uint32_t n = gradient_breakpoints(g, 0.0f, tmax, bp);
  const float rmax = (radii.x > radii.y ? radii.x : radii.y) * tmax;
  uint32_t segments = 16 + static_cast<uint32_t>(raster::clampf(rmax / 8.0f, 0.0f, 112.0f));
  float cs_[129], sn_[129];  // 16 + 112 = 128 segments at most
  for (uint32_t k = 0; k <= segments; ++k) {
    const double angle = 6.28318530717958647692 * k / segments;
    cs_[k] = static_cast<float>(fmath::cos(angle));
    sn_[k] = static_cast<float>(fmath::sin(angle));
  }
  // Rings are circumscribed polygons so the outermost one covers the whole rectangle.
  const float grow = static_cast<float>(1.0 / fmath::cos(3.14159265358979323846 / segments));
  const Rect2D saved_clip = clip_;
  close_range();
  clip_ = area;  // the rings overshoot the rectangle; the scissor trims them
  for (uint32_t i = 0; i + 1 < n; ++i) {
    const float lo = bp[i], hi = bp[i + 1];
    if (!(hi > lo)) continue;
    const uint32_t seg = g.segment_of((lo + hi) * 0.5f);
    const Vec4 c_lo = unpack_color(g.color_in_segment(seg, lo)), c_hi = unpack_color(g.color_in_segment(seg, hi));
    for (uint32_t k = 0; k < segments; ++k) {
      const float hx0 = center.x + radii.x * hi * grow * cs_[k], hy0 = center.y + radii.y * hi * grow * sn_[k];
      const float hx1 = center.x + radii.x * hi * grow * cs_[k + 1], hy1 = center.y + radii.y * hi * grow * sn_[k + 1];
      if (lo <= 0) {
        const float xs[3] = {center.x, hx0, hx1}, ys[3] = {center.y, hy0, hy1};
        const Vec4 cs[3] = {c_lo, c_hi, c_hi};
        triangle_colors(xs, ys, cs);
      } else {
        const float lx0 = center.x + radii.x * lo * grow * cs_[k], ly0 = center.y + radii.y * lo * grow * sn_[k];
        const float lx1 = center.x + radii.x * lo * grow * cs_[k + 1], ly1 = center.y + radii.y * lo * grow * sn_[k + 1];
        const float xa[3] = {lx0, hx0, hx1}, ya[3] = {ly0, hy0, hy1};
        const Vec4 ca[3] = {c_lo, c_hi, c_hi};
        triangle_colors(xa, ya, ca);
        const float xb[3] = {lx0, hx1, lx1}, yb[3] = {ly0, hy1, ly1};
        const Vec4 cb[3] = {c_lo, c_hi, c_lo};
        triangle_colors(xb, yb, cb);
      }
    }
  }
  close_range();
  clip_ = saved_clip;
}

bool DeviceRenderer2D::push_clip_rect(const Rect2D& rect) {
  if (clip_depth_ >= kMaxClipDepth) {
    ++clip_overflow_;
    apply_clip();
    return false;
  }
  clip_stack_[clip_depth_++] = raster::intersect(rect, {0, 0, extent_.width, extent_.height});
  apply_clip();
  return true;
}

// There is no stencil or coverage mask in the triangle pipeline: the bounding rectangle has to do.
bool DeviceRenderer2D::push_clip_rounded_rect(const Rect2D& rect, const CornerRadii& radii) {
  const raster::RRect rr = raster::RRect::make(rect, radii);
  bool square = true;
  for (int i = 0; i < 4; ++i) square = square && rr.rx[i] == 0;
  const bool ok = push_clip_rect(rect);
  return ok && square;
}

bool DeviceRenderer2D::push_clip_mask(const MaskView& mask, int32_t x, int32_t y) {
  if (!mask.valid()) {
    push_clip_rect({0, 0, 0, 0});
    return false;
  }
  push_clip_rect({x, y, mask.width, mask.height});
  return false;
}

void DeviceRenderer2D::pop_clip() {
  if (clip_overflow_) --clip_overflow_;
  else if (clip_depth_) --clip_depth_;
  apply_clip();
}

bool DeviceRenderer2D::push_layer(const Rect2D& bounds, uint8_t opacity) {
  if (layer_depth_ >= kMaxLayerDepth) {
    ++layer_overflow_;
    return false;
  }
  layer_clip_depth_[layer_depth_] = clip_depth_;
  layer_clip_overflow_[layer_depth_] = clip_overflow_;
  layer_opacity_[layer_depth_++] = opacity_;
  opacity_ *= static_cast<float>(opacity) / 255.0f;
  (void)bounds;  // (no offscreen target: nothing is isolated, so the bounds clip nothing)
  return opacity == 255;
}

void DeviceRenderer2D::pop_layer() {
  if (layer_overflow_) {
    --layer_overflow_;
    return;
  }
  if (!layer_depth_) return;
  --layer_depth_;
  opacity_ = layer_opacity_[layer_depth_];
  clip_depth_ = layer_clip_depth_[layer_depth_];  // clips pushed inside the layer end with it
  clip_overflow_ = layer_clip_overflow_[layer_depth_];
  apply_clip();
}

bool DeviceRenderer2D::copy_rect(const Rect2D& source, int32_t dest_x, int32_t dest_y) {
  if (!valid_) return false;
  const Rect2D src = raster::intersect(source, {0, 0, extent_.width, extent_.height});
  if (!src.width || !src.height) return true;
  if (!submit_pending() || !images_[back_]) return false;
  ArrayView<uint32_t> frame = allocate_view<uint32_t>(allocator_, uint64_t{extent_.width} * extent_.height);
  if (frame.empty()) return false;
  bool ok = images_[back_]->read_pixels(frame);
  if (ok) {
    Surface snapshot;
    snapshot.pixels = frame;
    snapshot.width = snapshot.pitch = extent_.width;
    snapshot.height = extent_.height;
    const BlendMode blend = blend_;
    const float opacity = opacity_;
    set_blend(BlendMode::Replace);
    opacity_ = 1.0f;
    draw_image(snapshot, src, raster::saturate_i32(int64_t{dest_x} + src.x - source.x), raster::saturate_i32(int64_t{dest_y} + src.y - source.y));
    opacity_ = opacity;
    set_blend(blend);
    close_range();  // the rectangles reference nothing in `snapshot`, but keep the modes tidy
  }
  release_view(allocator_, frame);
  return ok;
}

void DeviceRenderer2D::present() {
  if (submit_pending() && device_.present(images_[back_])) back_ ^= 1;
}

bool DeviceRenderer2D::read_pixels(ArrayView<uint32_t> out) { return submit_pending() && images_[back_] && images_[back_]->read_pixels(out); }

}  // namespace graphics
