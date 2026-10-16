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
#include "Hardware/GraphicsProcessingUnit/Font8x16.hpp"

namespace graphics {

DeviceRenderer2D::DeviceRenderer2D(IGraphicsDevice& device, allocators::IAllocator& allocator, Extent2D extent)
    : device_(device), allocator_(allocator), extent_(extent) {
  clip_ = {0, 0, extent.width, extent.height};
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
  image_ = device.create_image({extent, Format::B8G8R8A8Unorm, kImageColorAttachment | kImagePresent});
  commands_ = device.create_command_buffer();
  valid_ = blended_ && opaque_ && vertices_ && image_ && commands_;
  if (valid_) {
    batch_ = vertices_->map().cast<ColoredVertex>();
    valid_ = !batch_.empty();
  }
}

DeviceRenderer2D::~DeviceRenderer2D() {
  if (commands_) device_.destroy_command_buffer(commands_);
  if (image_) device_.destroy_image(image_);
  if (vertices_) device_.destroy_buffer(vertices_);
  if (opaque_) device_.destroy_pipeline(opaque_);
  if (blended_) device_.destroy_pipeline(blended_);
  if (fs_) device_.destroy_shader_module(fs_);
  if (vs_) device_.destroy_shader_module(vs_);
}

bool DeviceRenderer2D::flush() {
  if (!valid_ || count_ == 0) return true;
  vertices_->unmap();
  commands_->begin();
  RenderPassBegin pass;
  pass.color = image_;
  pass.clear_color = false;  // keep what earlier frames and flushes drew
  commands_->begin_render_pass(pass);
  commands_->set_scissor(clip_);
  commands_->bind_pipeline(blend_ == BlendMode::Alpha ? blended_ : opaque_);
  commands_->bind_vertex_buffer(vertices_);
  // Pixel coordinates (0,0 top left) to clip space; depth is constant.
  ColoredConstants constants;
  constants.mvp.m[0] = 2.0f / extent_.width;
  constants.mvp.m[5] = 2.0f / extent_.height;
  constants.mvp.m[10] = 0;
  constants.mvp.m[12] = -1;
  constants.mvp.m[13] = -1;
  constants.mvp.m[14] = 0.5f;
  commands_->push_constants(&constants, sizeof(constants));
  commands_->draw(count_);
  commands_->end_render_pass();
  const bool ok = commands_->end() && device_.submit(commands_);
  count_ = 0;
  return ok;
}

void DeviceRenderer2D::triangle(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t color) {
  if (!valid_) return;
  if (count_ + 3 > kMaxVertices) flush();
  const Vec4 c = unpack_color(color);
  const float xs[3] = {x0, x1, x2}, ys[3] = {y0, y1, y2};
  for (int i = 0; i < 3; ++i) batch_[count_++] = {{xs[i], ys[i], 0}, {c.x, c.y, c.z, c.w}};
}

void DeviceRenderer2D::quad(float x0, float y0, float x1, float y1, uint32_t color) {
  triangle(x0, y0, x1, y0, x1, y1, color);
  triangle(x0, y0, x1, y1, x0, y1, color);
}

void DeviceRenderer2D::set_clip(const Rect2D& clip) {
  flush();  // what is batched belongs to the old clip
  clip_ = clip;
}

void DeviceRenderer2D::reset_clip() {
  flush();
  clip_ = {0, 0, extent_.width, extent_.height};
}

void DeviceRenderer2D::set_blend(BlendMode mode) {
  if (mode != blend_) flush();
  blend_ = mode;
}

void DeviceRenderer2D::clear(uint32_t color) {
  if (!valid_) return;
  count_ = 0;  // the clear wipes whatever was batched
  commands_->begin();
  RenderPassBegin pass;
  pass.color = image_;
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

void DeviceRenderer2D::draw_image(const Surface& s, const Rect2D& rect, int32_t x, int32_t y) {
  if (!s.valid()) return;
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
      fill_rect({x + static_cast<int32_t>(i), y + static_cast<int32_t>(j), run, 1}, pixel);
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

void DeviceRenderer2D::present() {
  if (flush()) device_.present(image_);
}

bool DeviceRenderer2D::read_pixels(ArrayView<uint32_t> out) { return flush() && image_ && image_->read_pixels(out); }

}  // namespace graphics
