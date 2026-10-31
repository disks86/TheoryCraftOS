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
    sheet_vertices_ = device.create_buffer(sizeof(TexturedVertex) * kMaxSheetVertices, kBufferVertex);
    sheet_ = device.create_image({{kSheetSize, kSheetSize}, Format::B8G8R8A8Unorm, kImageSampled});
    sheet_pixels_ = allocate_view<uint32_t>(allocator, uint64_t{kSheetSize} * kSheetSize);
  }
  // Offscreen groups: custom blending (premultiplied over) and the shader that scales a texel; best effort, without them begin_group() says no.
  if ((device.info().features & kFeatureBlendState) && vs_ && fs_ && texture_vs_ && texture_fs_ && texture_vertices_) {
    BlendState over;   // source over, for a destination that holds premultiplied colors: rgb = src.rgb * a + dst.rgb * (1 - a), a = a + dst.a * (1 - a)
    over.custom = over.enable = true;
    over.src_color = BlendFactor::SrcAlpha;
    over.dst_color = BlendFactor::InvSrcAlpha;
    over.src_alpha = BlendFactor::One;
    over.dst_alpha = BlendFactor::InvSrcAlpha;
    BlendState premultiplied = over;   // a source that is premultiplied already
    premultiplied.src_color = BlendFactor::One;
    GraphicsPipelineDesc colored;
    describe_colored_pipeline(&colored);
    colored.vertex_shader = vs_;
    colored.fragment_shader = fs_;
    colored.cull_mode = CullMode::None;
    colored.blend = over;
    group_blended_ = device.create_graphics_pipeline(colored);
    GraphicsPipelineDesc textured;
    describe_textured_pipeline(&textured);
    textured.vertex_shader = texture_vs_;
    textured.fragment_shader = texture_fs_;
    textured.cull_mode = CullMode::None;
    textured.blend = over;
    texture_group_ = device.create_graphics_pipeline(textured);
    scaled_fs_ = device.create_shader_module(textured_scaled_fragment_shader());
    if (scaled_fs_) {
      textured.fragment_shader = scaled_fs_;
      textured.blend = premultiplied;
      composite_ = device.create_graphics_pipeline(textured);
    }
    groups_ok_ = group_blended_ && texture_group_ && composite_;
    // clips by a coverage mask: a group through a second texture's alpha
    masked_fs_ = device.create_shader_module(textured_masked_fragment_shader());
    if (masked_fs_ && groups_ok_) {
      textured.fragment_shader = masked_fs_;
      textured.texture_count = 2;
      textured.blend = premultiplied;
      masked_ = device.create_graphics_pipeline(textured);
      textured.texture_count = 1;
    }
    masks_ok_ = groups_ok_ && masked_;
    // the filters: one pass each, no blending (the result replaces the picture)
    blur_fs_ = device.create_shader_module(textured_blur_fragment_shader());
    color_fs_ = device.create_shader_module(textured_color_fragment_shader());
    textured.blend = BlendState();
    textured.alpha_blend = false;
    if (blur_fs_) {
      textured.fragment_shader = blur_fs_;
      filter_blur_ = device.create_graphics_pipeline(textured);
    }
    if (color_fs_) {
      textured.fragment_shader = color_fs_;
      filter_color_ = device.create_graphics_pipeline(textured);
    }
    shadow_fs_ = device.create_shader_module(textured_shadow_fragment_shader());
    if (shadow_fs_) {
      textured.fragment_shader = shadow_fs_;
      filter_shadow_ = device.create_graphics_pipeline(textured);
    }
    BlendState erase;   // dst = dst * (1 - src alpha), colour and alpha
    erase.custom = erase.enable = true;
    erase.src_color = erase.src_alpha = BlendFactor::Zero;
    erase.dst_color = erase.dst_alpha = BlendFactor::InvSrcAlpha;
    colored.blend = erase;
    group_erase_ = device.create_graphics_pipeline(colored);
    filters_ok_ = groups_ok_ && filter_blur_ && filter_color_ && group_erase_ && filter_shadow_;
  }
  // The glyph atlas (best effort: without it glyphs go the way of any mask)
  if (texture_vs_ && (device.info().features & kFeatureBlendState)) {
    tint_vs_ = device.create_shader_module(tinted_vertex_shader());
    tint_fs_ = device.create_shader_module(tinted_fragment_shader());
    if (tint_vs_ && tint_fs_) {
      GraphicsPipelineDesc tinted;
      describe_tinted_pipeline(&tinted);
      tinted.vertex_shader = tint_vs_;
      tinted.fragment_shader = tint_fs_;
      tinted.cull_mode = CullMode::None;
      tinted.alpha_blend = true;
      glyph_blended_ = device.create_graphics_pipeline(tinted);
      BlendState over;   // as the group pipelines: premultiplied over
      over.custom = over.enable = true;
      over.src_color = BlendFactor::SrcAlpha;
      over.dst_color = BlendFactor::InvSrcAlpha;
      over.src_alpha = BlendFactor::One;
      over.dst_alpha = BlendFactor::InvSrcAlpha;
      tinted.alpha_blend = false;
      tinted.blend = over;
      glyph_group_ = device.create_graphics_pipeline(tinted);
      glyph_vertices_ = device.create_buffer(sizeof(TintedVertex) * kMaxGlyphVertices, kBufferVertex);
      atlas_ = device.create_image({{kAtlasSize, kAtlasSize}, Format::B8G8R8A8Unorm, kImageSampled});
      atlas_pixels_ = allocate_view<uint32_t>(allocator, uint64_t{kAtlasSize} * kAtlasSize);
      atlas_table_ = allocate_view<AtlasEntry>(allocator, kAtlasTable);
    }
  }
  for (IImage*& image : images_) image = device.create_image({extent, Format::B8G8R8A8Unorm, kImageColorAttachment | kImagePresent});
  commands_ = device.create_command_buffer();
  valid_ = blended_ && opaque_ && vertices_ && images_[0] && images_[1] && commands_;
  if (valid_) {
    batch_ = vertices_->map().cast<ColoredVertex>();
    valid_ = !batch_.empty();
  }
  if (valid_ && glyph_blended_ && glyph_group_ && glyph_vertices_ && atlas_ && !atlas_pixels_.empty() && !atlas_table_.empty()) {
    glyph_batch_ = glyph_vertices_->map().cast<TintedVertex>();
    atlas_ok_ = glyph_batch_.size() >= kMaxGlyphVertices;
    for (uint32_t& p : atlas_pixels_) p = 0;
    for (AtlasEntry& e : atlas_table_) e = AtlasEntry();
  }
  if (valid_ && sheet_vertices_ && sheet_ && !sheet_pixels_.empty()) {
    sheet_batch_ = sheet_vertices_->map().cast<TexturedVertex>();
    sheet_ok_ = sheet_batch_.size() >= kMaxSheetVertices && texture_blended_ && texture_opaque_ && texture_vertices_;
    for (uint32_t& p : sheet_pixels_) p = 0;
  }
}

DeviceRenderer2D::~DeviceRenderer2D() {
  if (atlas_) device_.destroy_image(atlas_);
  if (glyph_vertices_) device_.destroy_buffer(glyph_vertices_);
  if (glyph_group_) device_.destroy_pipeline(glyph_group_);
  if (glyph_blended_) device_.destroy_pipeline(glyph_blended_);
  if (tint_fs_) device_.destroy_shader_module(tint_fs_);
  if (tint_vs_) device_.destroy_shader_module(tint_vs_);
  release_view(allocator_, atlas_pixels_);
  release_view(allocator_, atlas_table_);
  while (group_depth_) {
    if (groups_[--group_depth_].mask && groups_[group_depth_].mask_slot < 0) device_.destroy_image(groups_[group_depth_].mask);
    if (groups_[group_depth_].image) device_.destroy_image(groups_[group_depth_].image);
  }
  for (MaskEntry& e : mask_cache_)
    if (e.image) device_.destroy_image(e.image);
  if (filter_shadow_) device_.destroy_pipeline(filter_shadow_);
  if (shadow_fs_) device_.destroy_shader_module(shadow_fs_);
  if (group_erase_) device_.destroy_pipeline(group_erase_);
  if (masked_) device_.destroy_pipeline(masked_);
  if (masked_fs_) device_.destroy_shader_module(masked_fs_);
  if (filter_color_) device_.destroy_pipeline(filter_color_);
  if (filter_blur_) device_.destroy_pipeline(filter_blur_);
  if (color_fs_) device_.destroy_shader_module(color_fs_);
  if (blur_fs_) device_.destroy_shader_module(blur_fs_);
  if (composite_) device_.destroy_pipeline(composite_);
  if (texture_group_) device_.destroy_pipeline(texture_group_);
  if (group_blended_) device_.destroy_pipeline(group_blended_);
  if (scaled_fs_) device_.destroy_shader_module(scaled_fs_);
  if (sheet_) device_.destroy_image(sheet_);
  if (sheet_vertices_) device_.destroy_buffer(sheet_vertices_);
  release_view(allocator_, sheet_pixels_);
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
  close_sheet_range();
  close_glyph_range();
  if (count_ <= range_start_) return;
  ranges_[range_count_++] = {range_start_, count_ - range_start_, clip_, blend_, false, false, erase_};
  range_start_ = count_;
  if (range_count_ == kMaxRanges) submit_pending();
}

void DeviceRenderer2D::close_sheet_range() {
  if (sheet_count_ <= sheet_range_start_) return;
  ranges_[range_count_++] = {sheet_range_start_, sheet_count_ - sheet_range_start_, clip_, blend_, true, false, false};
  sheet_range_start_ = sheet_count_;
  if (range_count_ == kMaxRanges) submit_pending();
}

void DeviceRenderer2D::close_glyph_range() {
  if (glyph_count_ <= glyph_range_start_) return;
  ranges_[range_count_++] = {glyph_range_start_, glyph_count_ - glyph_range_start_, clip_, blend_, false, true, false};
  glyph_range_start_ = glyph_count_;
  if (range_count_ == kMaxRanges) submit_pending();
}

// A new frame starts with an empty sheet (what the last frame put on it is drawn by now).
void DeviceRenderer2D::reset_sheet() {
  sheet_x_ = sheet_y_ = sheet_shelf_ = 0;
}

IImage* DeviceRenderer2D::target_image() const { return group_depth_ ? groups_[group_depth_ - 1].image : images_[back_]; }

Extent2D DeviceRenderer2D::target_extent() const {
  if (!group_depth_) return extent_;
  const Rect2D& b = groups_[group_depth_ - 1].bounds;
  return {b.width, b.height};
}

// Drawing is in the frame's coordinates whatever the target: a group's image starts at its bounds, so shapes and scissors are moved by that.
Rect2D DeviceRenderer2D::to_target(const Rect2D& clip) const {
  if (!group_depth_) return clip;
  const Rect2D& b = groups_[group_depth_ - 1].bounds;
  const Rect2D inside = raster::intersect(clip, b);
  return {inside.x - b.x, inside.y - b.y, inside.width, inside.height};
}

// Pixel coordinates (0,0 top left of the image) to clip space; depth is constant.
void DeviceRenderer2D::fill_matrix(Mat4* mvp) const {
  const Extent2D e = target_extent();
  const float ox = group_depth_ ? static_cast<float>(groups_[group_depth_ - 1].bounds.x) : 0.0f, oy = group_depth_ ? static_cast<float>(groups_[group_depth_ - 1].bounds.y) : 0.0f;
  mvp->m[0] = 2.0f / e.width;
  mvp->m[5] = 2.0f / e.height;
  mvp->m[10] = 0;
  mvp->m[12] = -1 - 2.0f * ox / e.width;
  mvp->m[13] = -1 - 2.0f * oy / e.height;
  mvp->m[14] = 0.5f;
}

IPipeline* DeviceRenderer2D::colored_pipeline(BlendMode mode) const { return mode == BlendMode::Alpha ? (group_depth_ ? group_blended_ : blended_) : opaque_; }
IPipeline* DeviceRenderer2D::textured_pipeline(BlendMode mode) const { return mode == BlendMode::Alpha ? (group_depth_ ? texture_group_ : texture_blended_) : texture_opaque_; }

bool DeviceRenderer2D::submit_pending() {
  close_range();
  if (!valid_ || range_count_ == 0) return true;
  vertices_->unmap();
  if (atlas_ok_) {
    glyph_vertices_->unmap();
    if (atlas_dirty_) {
      if (!upload(atlas_, atlas_pixels_, kAtlasSize, atlas_dirty_area_, &atlas_pixels_sent_)) return false;
      atlas_dirty_ = false;
    }
  }
  if (sheet_ok_) {
    sheet_vertices_->unmap();
    if (sheet_dirty_) {
      uint64_t sent = 0;
      if (!upload(sheet_, sheet_pixels_, kSheetSize, sheet_dirty_area_, &sent)) return false;
      sheet_dirty_ = false;
    }
  }
  commands_->begin();
  RenderPassBegin pass;
  pass.color = target_image();
  pass.clear_color = false;  // keep what earlier frames and submissions drew
  commands_->begin_render_pass(pass);
  ColoredConstants constants;
  fill_matrix(&constants.mvp);
  bool pushed = false;
  int bound = -1;   // which vertex buffer is bound: 0 colored, 1 the sheet's, 2 the glyphs'
  for (uint32_t i = 0; i < range_count_; ++i) {
    const Range& r = ranges_[i];
    commands_->set_scissor(to_target(r.clip));
    if (r.glyph) {
      commands_->bind_pipeline(group_depth_ ? glyph_group_ : glyph_blended_);
      if (bound != 2) {
        commands_->bind_vertex_buffer(glyph_vertices_);
        SamplerDesc sampler;   // (nearest at the pixel centres: the coverage as it is)
        sampler.address_u = sampler.address_v = AddressMode::ClampToEdge;
        commands_->bind_texture(0, atlas_, sampler);
        bound = 2;
      }
    } else if (r.sheet) {
      commands_->bind_pipeline(textured_pipeline(r.blend));
      if (bound != 1) {
        commands_->bind_vertex_buffer(sheet_vertices_);
        SamplerDesc sampler;   // (nearest at the pixel centres: the baked pixels as they are)
        sampler.address_u = sampler.address_v = AddressMode::ClampToEdge;
        commands_->bind_texture(0, sheet_, sampler);
        bound = 1;
      }
    } else {
      commands_->bind_pipeline(r.erase && group_erase_ ? group_erase_ : colored_pipeline(r.blend));
      if (bound != 0) {
        commands_->bind_vertex_buffer(vertices_);
        bound = 0;
      }
    }
    if (!pushed) {  // (constants stay with the commands; binding another pipeline does not lose them)
      commands_->push_constants(&constants, sizeof(constants));
      pushed = true;
    }
    commands_->draw(r.count, r.first);
  }
  commands_->end_render_pass();
  const bool ok = commands_->end() && device_.submit(commands_);
  count_ = range_start_ = range_count_ = 0;
  sheet_count_ = sheet_range_start_ = 0;
  glyph_count_ = glyph_range_start_ = 0;
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
  if (sheet_count_ > sheet_range_start_) close_sheet_range();   // (a run of sheet rectangles ends here: the order of drawing is kept)
  if (glyph_count_ > glyph_range_start_) close_glyph_range();
  for (int i = 0; i < 3; ++i) batch_[count_++] = {{xs[i], ys[i], 0}, {colors[i].x, colors[i].y, colors[i].z, colors[i].w * opacity_}};
}

void DeviceRenderer2D::quad(float x0, float y0, float x1, float y1, uint32_t color) {
  triangle(x0, y0, x1, y0, x1, y1, color);
  triangle(x0, y0, x1, y1, x0, y1, color);
}

void DeviceRenderer2D::apply_clip() {
  close_range();  // what is recorded belongs to the old clip
  Rect2D r = base_clip_;
  uint32_t first = 0;
  if (detach_index_ != kNoDetach && detach_index_ < clip_depth_) {   // (inside a filter group the clips outside it do not apply)
    r = clip_stack_[detach_index_];
    first = detach_index_ + 1;
  }
  for (uint32_t i = first; i < clip_depth_; ++i) r = raster::intersect(r, clip_stack_[i]);
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
  if (group_depth_) {   // (a group is cleared by painting over it)
    const Rect2D b = groups_[group_depth_ - 1].bounds;
    const BlendMode saved = blend_;
    set_blend(BlendMode::Replace);
    quad(static_cast<float>(b.x), static_cast<float>(b.y), static_cast<float>(b.x) + b.width, static_cast<float>(b.y) + b.height, color);
    set_blend(saved);
    return;
  }
  count_ = range_start_ = range_count_ = 0;  // the clear wipes whatever was recorded
  sheet_count_ = sheet_range_start_ = 0;
  glyph_count_ = glyph_range_start_ = 0;
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
  return upload_and_draw(w, h, dx, dy);
}

bool DeviceRenderer2D::prepare_rows(uint64_t count) {
  if (texture_rows_.size() >= count) return true;
  release_view(allocator_, texture_rows_);
  texture_rows_ = allocate_view<uint32_t>(allocator_, count);
  return !texture_rows_.empty();
}

// The first w * h pixels of texture_rows_ as one rectangle at (dx, dy): uploaded as a texture of that size and drawn after what was recorded before it.
bool DeviceRenderer2D::upload_and_draw(uint64_t w, uint64_t h, int64_t dx, int64_t dy) {
  const uint32_t width = static_cast<uint32_t>(w), height = static_cast<uint32_t>(h);
  if (!submit_pending() || !ensure_texture(width, height) || !texture_->write_pixels(ArrayView<const uint32_t>(texture_rows_.data(), w * h))) return false;
  return draw_textured_quad(*texture_, dx, dy, width, height, 0.0f, 0.0f, 1.0f, 1.0f);
}

bool DeviceRenderer2D::sheet_slot(uint32_t w, uint32_t h, uint32_t* x, uint32_t* y) {
  if (w > kSheetSize || h > kSheetSize) return false;
  if (sheet_x_ + w > kSheetSize) {   // the next shelf
    sheet_y_ += sheet_shelf_;
    sheet_x_ = 0;
    sheet_shelf_ = 0;
  }
  if (sheet_y_ + h > kSheetSize) {   // full: what was recorded is drawn with the sheet as it is, and the sheet starts again
    if (!submit_pending()) return false;
    reset_sheet();
  }
  *x = sheet_x_;
  *y = sheet_y_;
  sheet_x_ += w;
  if (h > sheet_shelf_) sheet_shelf_ = h;
  return true;
}

void DeviceRenderer2D::sheet_emit(uint32_t sx, uint32_t sy, uint32_t w, uint32_t h, int64_t dx, int64_t dy) {
  if (clip_.width == 0 || clip_.height == 0) return;
  if (sheet_count_ + 6 > kMaxSheetVertices) submit_pending();   // (the sheet's contents stay: the slot is already taken)
  if (count_ > range_start_ || glyph_count_ > glyph_range_start_) close_range();   // (a run of colored shapes or glyphs ends here)
  constexpr float k = 1.0f / kSheetSize;
  const float x0 = static_cast<float>(dx), y0 = static_cast<float>(dy), x1 = x0 + static_cast<float>(w), y1 = y0 + static_cast<float>(h);
  const float u0 = static_cast<float>(sx) * k, v0 = static_cast<float>(sy) * k, u1 = static_cast<float>(sx + w) * k, v1 = static_cast<float>(sy + h) * k;
  const TexturedVertex quad[6] = {{{x0, y0, 0}, {u0, v0}}, {{x1, y0, 0}, {u1, v0}}, {{x1, y1, 0}, {u1, v1}}, {{x0, y0, 0}, {u0, v0}}, {{x1, y1, 0}, {u1, v1}}, {{x0, y1, 0}, {u0, v1}}};
  for (const TexturedVertex& v : quad) sheet_batch_[sheet_count_++] = v;
}

// A mask in the colors coverage_color gives it (alpha blending only), as pixels: small ones are baked onto the sheet, big ones are a texture of their own.
bool DeviceRenderer2D::draw_mask_textured(const MaskView& mask, int64_t ddx, int64_t ddy, int64_t dx0, int64_t dy0, uint64_t w, uint64_t h, uint32_t color) {
  if (!sheet_ok_) return false;
  const bool small = w <= kSheetMaxMask && h <= kSheetMaxMask && w * h < kTextureMinPixels;
  uint32_t sx = 0, sy = 0;
  if (small) {
    if (!sheet_slot(static_cast<uint32_t>(w), static_cast<uint32_t>(h), &sx, &sy)) return false;
  } else if (!prepare_rows(w * h)) {
    return false;
  }
  for (uint64_t j = 0; j < h; ++j) {
    const ConstByteView cov = mask.coverage.subview(static_cast<uint64_t>(dy0 - ddy + static_cast<int64_t>(j)) * mask.pitch + static_cast<uint64_t>(dx0 - ddx), w);
    const ArrayView<uint32_t> out = small ? sheet_pixels_.subview(uint64_t{sy + j} * kSheetSize + sx, w) : texture_rows_.subview(j * w, w);
    for (uint64_t i = 0; i < w; ++i) {
      uint32_t c;
      out[i] = coverage_color(color, cov[i], &c) ? c : 0;   // (no coverage: fully transparent, which blends to nothing)
    }
  }
  if (!small) return upload_and_draw(w, h, dx0, dy0);
  grow_area(&sheet_dirty_area_, &sheet_dirty_, static_cast<int32_t>(sx), static_cast<int32_t>(sy), static_cast<uint32_t>(w), static_cast<uint32_t>(h));
  sheet_dirty_ = true;
  sheet_emit(sx, sy, static_cast<uint32_t>(w), static_cast<uint32_t>(h), dx0, dy0);
  return true;
}

// One rectangle at (dx, dy), w x h pixels, showing the part (u0, v0)-(u1, v1) of the image: drawn at once, in a submit of its own after what was recorded before it.
bool DeviceRenderer2D::draw_textured_quad(IImage& image, int64_t dx, int64_t dy, uint32_t w, uint32_t h, float u0, float v0, float u1, float v1) {
  const float x0 = static_cast<float>(dx), y0 = static_cast<float>(dy), x1 = x0 + static_cast<float>(w), y1 = y0 + static_cast<float>(h);
  const float xs[4] = {x0, x1, x1, x0}, ys[4] = {y0, y0, y1, y1};
  return draw_textured_corners(image, xs, ys, u0, v0, u1, v1, Filter::Nearest);   // (nearest at the pixel centres: the picture's own pixels)
}

bool DeviceRenderer2D::draw_textured_corners(IImage& image, const float xs[4], const float ys[4], float u0, float v0, float u1, float v1, Filter filter, const float* scale, IImage* mask, AddressMode address) {
  if (!submit_pending()) return false;
  const TexturedVertex tl{{xs[0], ys[0], 0}, {u0, v0}}, tr{{xs[1], ys[1], 0}, {u1, v0}}, br{{xs[2], ys[2], 0}, {u1, v1}}, bl{{xs[3], ys[3], 0}, {u0, v1}};
  const TexturedVertex quad[6] = {tl, tr, br, tl, br, bl};
  const ArrayView<TexturedVertex> mapped = texture_vertices_->map().cast<TexturedVertex>();
  if (mapped.size() < 6) return false;
  mapped.copy_from(ArrayView<const TexturedVertex>(quad, 6));
  texture_vertices_->unmap();

  commands_->begin();
  RenderPassBegin pass;
  pass.color = target_image();
  pass.clear_color = false;
  commands_->begin_render_pass(pass);
  commands_->bind_pipeline(mask ? masked_ : scale ? composite_ : textured_pipeline(blend_));
  commands_->bind_vertex_buffer(texture_vertices_);
  SamplerDesc sampler;
  sampler.filter = filter;
  sampler.address_u = sampler.address_v = address;
  commands_->bind_texture(0, &image, sampler);
  if (mask) {
    SamplerDesc nearest;   // (the coverage as it is, at the pixel centres)
    nearest.address_u = nearest.address_v = AddressMode::ClampToEdge;
    commands_->bind_texture(1, mask, nearest);
  }
  TexturedScaledConstants constants;
  fill_matrix(&constants.mvp);
  constants.scale = scale ? *scale : 1.0f;
  commands_->push_constants(&constants, scale ? sizeof(constants) : sizeof(ColoredConstants));
  commands_->set_scissor(to_target(clip_));
  commands_->draw(6);
  commands_->end_render_pass();
  return commands_->end() && device_.submit(commands_);
}

// The tile as one texture, drawn as one rectangle over `area` whose texture coordinates run over the copies, the sampler repeating them.
bool DeviceRenderer2D::draw_image_tiled(const Surface& s, int32_t origin_x, int32_t origin_y, uint32_t cell_w, uint32_t cell_h, const Rect2D& area) {
  if (!valid_ || !s.valid() || !cell_w || !cell_h || !texture_blended_ || !texture_opaque_ || !texture_vertices_ || opacity_ != 1.0f || !s.width || !s.height) return false;
  const Rect2D shown = raster::intersect(area, clip_);
  if (!shown.width || !shown.height) return true;   // (nothing of it is inside the clip)
  const uint64_t count = uint64_t{s.width} * s.height;
  if (count > (uint64_t{1} << 24) || !prepare_rows(count)) return false;
  const ArrayView<uint32_t> rows = texture_rows_.first(count);
  for (uint32_t j = 0; j < s.height; ++j) {
    const ArrayView<const uint32_t> from = ArrayView<const uint32_t>(s.row(j), s.width);
    const ArrayView<uint32_t> to = rows.subview(uint64_t{j} * s.width, s.width);
    if (s.format == Format::B8G8R8A8Unorm) to.copy_from(from);
    else
      for (uint32_t i = 0; i < s.width; ++i) to[i] = s.decode(from[i]);
  }
  if (!submit_pending() || !ensure_texture(s.width, s.height) || !texture_->write_pixels(ArrayView<const uint32_t>(texture_rows_.data(), count))) return false;
  // texture coordinates: copies from the origin, folded into one period so that the numbers stay small
  auto fold = [](int64_t v, int64_t period) { const int64_t m = v % period; return m < 0 ? m + period : m; };
  const float u0 = static_cast<float>(fold(int64_t{shown.x} - origin_x, cell_w)) / static_cast<float>(cell_w);
  const float v0 = static_cast<float>(fold(int64_t{shown.y} - origin_y, cell_h)) / static_cast<float>(cell_h);
  const float u1 = u0 + static_cast<float>(shown.width) / static_cast<float>(cell_w), v1 = v0 + static_cast<float>(shown.height) / static_cast<float>(cell_h);
  const float x0 = static_cast<float>(shown.x), y0 = static_cast<float>(shown.y), x1 = x0 + static_cast<float>(shown.width), y1 = y0 + static_cast<float>(shown.height);
  const float xs[4] = {x0, x1, x1, x0}, ys[4] = {y0, y0, y1, y1};
  return draw_textured_corners(*texture_, xs, ys, u0, v0, u1, v1, Filter::Nearest, nullptr, nullptr, AddressMode::Repeat);
}

bool DeviceRenderer2D::draw_image_transformed(const Surface& s, const Rect2D& rect, const Matrix2D& m, ImageFilter filter) {
  if (!valid_ || !s.valid() || !texture_blended_ || !texture_opaque_ || !texture_vertices_ || opacity_ != 1.0f) return false;
  const float det = m.a * m.d - m.b * m.c;
  if (!(det > 1e-12f || det < -1e-12f)) return false;
  // the part of the picture that exists
  const int64_t sx0 = rect.x > 0 ? rect.x : 0, sy0 = rect.y > 0 ? rect.y : 0;
  const int64_t sx1 = int64_t{rect.x} + rect.width < s.width ? int64_t{rect.x} + rect.width : s.width;
  const int64_t sy1 = int64_t{rect.y} + rect.height < s.height ? int64_t{rect.y} + rect.height : s.height;
  if (sx0 >= sx1 || sy0 >= sy1) return true;
  const uint32_t w = static_cast<uint32_t>(sx1 - sx0), h = static_cast<uint32_t>(sy1 - sy0);
  const uint64_t count = uint64_t{w} * h;
  if (!prepare_rows(count)) return false;
  const ArrayView<uint32_t> rows = texture_rows_.first(count);
  for (uint32_t j = 0; j < h; ++j) {
    const ArrayView<const uint32_t> from = ArrayView<const uint32_t>(s.row(static_cast<uint32_t>(sy0) + j), s.width).subview(static_cast<uint64_t>(sx0), w);
    const ArrayView<uint32_t> to = rows.subview(uint64_t{j} * w, w);
    if (s.format == Format::B8G8R8A8Unorm) to.copy_from(from);
    else
      for (uint32_t i = 0; i < w; ++i) to[i] = s.decode(from[i]);
  }
  if (!submit_pending() || !ensure_texture(w, h) || !texture_->write_pixels(ArrayView<const uint32_t>(texture_rows_.data(), count))) return false;
  // the corners of what exists, measured from the rectangle's top left corner
  const float ox = static_cast<float>(sx0 - rect.x), oy = static_cast<float>(sy0 - rect.y), fw = static_cast<float>(w), fh = static_cast<float>(h);
  const float lx[4] = {ox, ox + fw, ox + fw, ox}, ly[4] = {oy, oy, oy + fh, oy + fh};
  float xs[4], ys[4];
  for (int i = 0; i < 4; ++i) {
    xs[i] = m.a * lx[i] + m.c * ly[i] + m.e;
    ys[i] = m.b * lx[i] + m.d * ly[i] + m.f;
  }
  return draw_textured_corners(*texture_, xs, ys, 0.0f, 0.0f, 1.0f, 1.0f, filter == ImageFilter::Nearest ? Filter::Nearest : Filter::Linear);
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
    // The set pixels as rectangles: a run of a row, taller while the rows below have the same pixels set (a stem is one rectangle, not one a row). The pixels covered are the glyph's.
    uint8_t rows[hardware::Font8x16::kHeight];
    for (uint32_t j = 0; j < hardware::Font8x16::kHeight; ++j) rows[j] = glyph[j];
    for (uint32_t j = 0; j < hardware::Font8x16::kHeight; ++j) {
      uint32_t i = 0;
      while (i < hardware::Font8x16::kWidth) {
        if (!(rows[j] & (0x80 >> i))) {
          ++i;
          continue;
        }
        uint32_t run = 1;
        while (i + run < hardware::Font8x16::kWidth && (rows[j] & (0x80 >> (i + run)))) ++run;
        uint8_t mask = 0;
        for (uint32_t k = 0; k < run; ++k) mask |= static_cast<uint8_t>(0x80 >> (i + k));
        uint32_t height = 1;
        while (j + height < hardware::Font8x16::kHeight && (rows[j + height] & mask) == mask) ++height;
        for (uint32_t k = 0; k < height; ++k) rows[j + k] &= static_cast<uint8_t>(~mask);
        fill_rect({x + static_cast<int32_t>(i), y + static_cast<int32_t>(j), run, height}, color);
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
  // (alpha blending at full opacity: glyphs and shadows go up as pixels, not as a rectangle per run of equal coverage)
  if (blend_ == BlendMode::Alpha && opacity_ == 1.0f && draw_mask_textured(mask, ddx, ddy, dx0, dy0, n, static_cast<uint64_t>(dy1 - dy0), color)) return;
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

void DeviceRenderer2D::atlas_reset() {
  atlas_x_ = atlas_y_ = atlas_shelf_ = 0;
  atlas_entries_ = 0;
  for (AtlasEntry& e : atlas_table_) e.used = false;
}

// The rectangle that holds `area` and the new part (when nothing was added since the last upload, `area` is just the new part).
void DeviceRenderer2D::grow_area(Rect2D* area, bool* dirty, int32_t x, int32_t y, uint32_t w, uint32_t h) {
  if (!*dirty) {
    *area = Rect2D{x, y, w, h};
    return;
  }
  const int32_t x0 = x < area->x ? x : area->x, y0 = y < area->y ? y : area->y;
  const int64_t x1 = int64_t{x} + w > int64_t{area->x} + area->width ? int64_t{x} + w : int64_t{area->x} + area->width;
  const int64_t y1 = int64_t{y} + h > int64_t{area->y} + area->height ? int64_t{y} + h : int64_t{area->y} + area->height;
  *area = Rect2D{x0, y0, static_cast<uint32_t>(x1 - x0), static_cast<uint32_t>(y1 - y0)};
}

bool DeviceRenderer2D::upload(IImage* image, ArrayView<uint32_t> pixels, uint32_t size, const Rect2D& area, uint64_t* sent) {
  const uint64_t offset = uint64_t{static_cast<uint32_t>(area.y)} * size + static_cast<uint32_t>(area.x);
  if (area.width && area.height && area.x >= 0 && area.y >= 0 && image->write_rect(area, ArrayView<const uint32_t>(pixels.subview(offset)), size)) {
    *sent += uint64_t{area.width} * area.height;
    return true;
  }
  *sent += uint64_t{size} * size;
  return image->write_pixels(ArrayView<const uint32_t>(pixels));
}

bool DeviceRenderer2D::atlas_slot(uint32_t w, uint32_t h, uint32_t* x, uint32_t* y) {
  if (atlas_x_ + w > kAtlasSize) {   // the next shelf
    atlas_y_ += atlas_shelf_;
    atlas_x_ = 0;
    atlas_shelf_ = 0;
  }
  if (atlas_y_ + h > kAtlasSize || atlas_entries_ >= kAtlasTable * 3 / 4) {   // full: what was recorded is drawn with the atlas as it is, and the atlas starts again
    if (!submit_pending()) return false;
    atlas_reset();
  }
  *x = atlas_x_;
  *y = atlas_y_;
  atlas_x_ += w;
  if (h > atlas_shelf_) atlas_shelf_ = h;
  return true;
}

// A glyph: its coverage is kept on the atlas (made and sent up the first time), and it is drawn as a rectangle tinted by the vertex color.
void DeviceRenderer2D::draw_glyph(uint64_t id, const MaskView& mask, int32_t x, int32_t y, uint32_t color) {
  if (!valid_) return;
  const bool usable = atlas_ok_ && blend_ == BlendMode::Alpha && !erase_ && mask.valid() && mask.width <= kAtlasMaxGlyph && mask.height <= kAtlasMaxGlyph;
  if (!usable) {
    draw_mask(mask, Rect2D{0, 0, mask.width, mask.height}, x, y, color);
    return;
  }
  if (clip_.width == 0 || clip_.height == 0 || (color >> 24) == 0) return;
  const Rect2D area = raster::intersect(Rect2D{x, y, mask.width, mask.height}, clip_);
  if (!area.width || !area.height) return;   // (nothing of it is inside the clip)
  // find it, or make a place for it
  uint32_t slot = static_cast<uint32_t>(id ^ (id >> 29)) % kAtlasTable;
  AtlasEntry* entry = nullptr;
  for (uint32_t probe = 0; probe < kAtlasTable; ++probe) {
    AtlasEntry& e = atlas_table_[(slot + probe) % kAtlasTable];
    if (!e.used) {
      entry = &e;
      break;
    }
    if (e.id == id && e.w == mask.width && e.h == mask.height) {
      entry = &e;
      break;
    }
  }
  if (!entry) return;   // (the table cannot be full: atlas_slot starts again at three quarters)
  if (!entry->used) {
    uint32_t ax = 0, ay = 0;
    if (!atlas_slot(mask.width, mask.height, &ax, &ay)) return;
    // (atlas_slot may have started the atlas again: the table was emptied, so look for the entry's place again)
    slot = static_cast<uint32_t>(id ^ (id >> 29)) % kAtlasTable;
    entry = nullptr;
    for (uint32_t probe = 0; probe < kAtlasTable; ++probe) {
      AtlasEntry& e = atlas_table_[(slot + probe) % kAtlasTable];
      if (!e.used) {
        entry = &e;
        break;
      }
    }
    if (!entry) return;
    for (uint32_t j = 0; j < mask.height; ++j) {
      const ConstByteView row = mask.coverage.subview(uint64_t{j} * mask.pitch, mask.width);
      const ArrayView<uint32_t> out = atlas_pixels_.subview(uint64_t{ay + j} * kAtlasSize + ax, mask.width);
      for (uint32_t i = 0; i < mask.width; ++i) out[i] = static_cast<uint32_t>(row[i]) << 24;
    }
    entry->id = id;
    entry->x = static_cast<uint16_t>(ax);
    entry->y = static_cast<uint16_t>(ay);
    entry->w = static_cast<uint16_t>(mask.width);
    entry->h = static_cast<uint16_t>(mask.height);
    entry->used = true;
    ++atlas_entries_;
    ++glyphs_made_;
    grow_area(&atlas_dirty_area_, &atlas_dirty_, static_cast<int32_t>(ax), static_cast<int32_t>(ay), mask.width, mask.height);
    atlas_dirty_ = true;
  }
  if (glyph_count_ + 6 > kMaxGlyphVertices) submit_pending();
  if (count_ > range_start_ || sheet_count_ > sheet_range_start_) close_range();   // (a run of colored shapes or sheet rectangles ends here: the order of drawing is kept)
  constexpr float k = 1.0f / kAtlasSize;
  const float x0 = static_cast<float>(x), y0 = static_cast<float>(y), x1 = x0 + static_cast<float>(entry->w), y1 = y0 + static_cast<float>(entry->h);
  const float u0 = static_cast<float>(entry->x) * k, v0 = static_cast<float>(entry->y) * k, u1 = static_cast<float>(entry->x + entry->w) * k, v1 = static_cast<float>(entry->y + entry->h) * k;
  const Vec4 c = unpack_color(color);
  const float tint[4] = {c.x, c.y, c.z, c.w * opacity_};
  auto vertex = [&](float px, float py, float u, float v) {
    TintedVertex t{{px, py, 0}, {u, v}, {tint[0], tint[1], tint[2], tint[3]}};
    glyph_batch_[glyph_count_++] = t;
  };
  vertex(x0, y0, u0, v0); vertex(x1, y0, u1, v0); vertex(x1, y1, u1, v1);
  vertex(x0, y0, u0, v0); vertex(x1, y1, u1, v1); vertex(x0, y1, u0, v1);
}

void DeviceRenderer2D::draw_image_scaled(const Surface& source, const RectF& src, const Rect2D& dest, ImageFilter filter) {
  if (!valid_ || !dest.width || !dest.height) return;
  const raster::SampleBounds bounds = raster::sample_bounds(source, src);
  if (bounds.empty()) return;
  const Rect2D area = raster::intersect(dest, clip_);
  if (!area.width || !area.height) return;
  const float step_x = src.width / static_cast<float>(dest.width), step_y = src.height / static_cast<float>(dest.height);
  const int64_t col0 = int64_t{area.x} - dest.x, row0 = int64_t{area.y} - dest.y;
  // A big enough area is sampled here, as ever, but sent up as one texture (the pixels are the same ones) instead of a rectangle per run.
  if (sheet_ok_ && opacity_ == 1.0f && uint64_t{area.width} * area.height >= kTextureMinPixels && prepare_rows(uint64_t{area.width} * area.height)) {
    for (uint32_t j = 0; j < area.height; ++j) {
      const float sy = src.y + (static_cast<float>(row0 + j) + 0.5f) * step_y;
      for (uint32_t i = 0; i < area.width; ++i) {
        const float sx = src.x + (static_cast<float>(col0 + i) + 0.5f) * step_x;
        texture_rows_[uint64_t{j} * area.width + i] = filter == ImageFilter::Nearest ? raster::sample_nearest(source, bounds, sx, sy) : raster::sample_bilinear(source, bounds, sx, sy);
      }
    }
    if (upload_and_draw(area.width, area.height, area.x, area.y)) return;
  }
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

// A fan of thin wedges around the center, each with the colors of its two edges (the center vertex has their mean, so that the color changes along a wedge by at most half the step).
bool DeviceRenderer2D::fill_conic_gradient(const Rect2D& rect, Vec2 center, float start_turns, ArrayView<const GradientStop> samples) {
  if (!valid_ || samples.size() < 2 || samples.size() > 4096) return false;
  const Rect2D area = raster::intersect(rect, clip_);
  if (!area.width || !area.height) return true;
  const float x0 = static_cast<float>(area.x), y0 = static_cast<float>(area.y), x1 = x0 + static_cast<float>(area.width), y1 = y0 + static_cast<float>(area.height);
  float far = 0;
  const float corner_x[2] = {x0, x1}, corner_y[2] = {y0, y1};
  for (const float cx : corner_x)
    for (const float cy : corner_y) {
      const float d = raster::sqrt_f((cx - center.x) * (cx - center.x) + (cy - center.y) * (cy - center.y));
      far = d > far ? d : far;
    }
  const float radius = far + 2.0f;
  constexpr double kTwoPi = 6.28318530717958647692;
  const Rect2D saved_clip = clip_;
  close_range();
  clip_ = area;   // (the wedges overshoot the rectangle; the scissor trims them)
  for (uint64_t i = 0; i + 1 < samples.size(); ++i) {
    const GradientStop& a = samples[i];
    const GradientStop& b = samples[i + 1];
    const float span = b.offset - a.offset;
    if (!(span > 1e-7f)) continue;   // (a hard edge: no area between the two samples)
    const Vec4 ca = unpack_color(a.color), cb = unpack_color(b.color);
    uint32_t parts = static_cast<uint32_t>(span * static_cast<float>(kTwoPi) / 0.4f) + 1;   // (a wedge is at most 0.4 radians wide: its chord stays inside the circumscribed radius)
    if (parts > 64) parts = 64;
    for (uint32_t k = 0; k < parts; ++k) {
      const float f0 = static_cast<float>(k) / static_cast<float>(parts), f1 = static_cast<float>(k + 1) / static_cast<float>(parts);
      const double t0 = kTwoPi * (static_cast<double>(start_turns) + a.offset + span * f0), t1 = kTwoPi * (static_cast<double>(start_turns) + a.offset + span * f1);
      const float chord = static_cast<float>(1.0 / fmath::cos((t1 - t0) * 0.5));
      const float xs[3] = {center.x, center.x + radius * chord * static_cast<float>(fmath::sin(t0)), center.x + radius * chord * static_cast<float>(fmath::sin(t1))};
      const float ys[3] = {center.y, center.y - radius * chord * static_cast<float>(fmath::cos(t0)), center.y - radius * chord * static_cast<float>(fmath::cos(t1))};
      auto mix = [&](float f) { return Vec4{ca.x + (cb.x - ca.x) * f, ca.y + (cb.y - ca.y) * f, ca.z + (cb.z - ca.z) * f, ca.w + (cb.w - ca.w) * f}; };
      const Vec4 c0 = mix(f0), c1 = mix(f1), mid = mix((f0 + f1) * 0.5f);
      const Vec4 cs[3] = {mid, c0, c1};
      triangle_colors(xs, ys, cs);
    }
  }
  close_range();
  clip_ = saved_clip;
  return true;
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
  if (!square && push_exact_clip(rect, &rr, nullptr)) return true;
  const bool ok = push_clip_rect(rect);
  return ok && square;
}

bool DeviceRenderer2D::push_clip_mask(const MaskView& mask, int32_t x, int32_t y) {
  if (!mask.valid()) {
    push_clip_rect({0, 0, 0, 0});
    return false;
  }
  const Rect2D area{x, y, mask.width, mask.height};
  if (push_exact_clip(area, nullptr, &mask)) return true;
  push_clip_rect(area);
  return false;
}

void DeviceRenderer2D::pop_clip() {
  if (clip_overflow_) {
    --clip_overflow_;
  } else if (group_depth_ && groups_[group_depth_ - 1].mask && groups_[group_depth_ - 1].clip_depth + 1 == clip_depth_) {
    finish_group(Matrix2D(), 255, ImageFilter::Nearest);   // (restores the clips below it)
    return;
  } else if (clip_depth_) {
    --clip_depth_;
  }
  apply_clip();
}

bool DeviceRenderer2D::push_layer(const Rect2D& bounds, uint8_t opacity) {
  if (layer_depth_ >= kMaxLayerDepth) {
    ++layer_overflow_;
    return false;
  }
  layer_clip_depth_[layer_depth_] = clip_depth_;
  layer_clip_overflow_[layer_depth_] = clip_overflow_;
  layer_opacity_[layer_depth_] = opacity_;
  layer_is_group_ &= ~(1u << layer_depth_);
  if (opacity != 255 && begin_group(bounds)) {   // isolated: drawn on its own and laid over the target as one
    groups_[group_depth_ - 1].layer = true;
    groups_[group_depth_ - 1].layer_opacity = opacity;
    layer_is_group_ |= 1u << layer_depth_;
    ++layer_depth_;
    return true;
  }
  if (opacity != 255 && groups_ok_ && clip_depth_ < kMaxClipDepth) {
    const Rect2D shown = raster::intersect(bounds, clip_);
    if (!shown.width || !shown.height) {   // nothing of it is on the target: nothing in it shows
      clip_stack_[clip_depth_++] = {0, 0, 0, 0};
      apply_clip();
      ++layer_depth_;
      return true;
    }
  }
  ++layer_depth_;
  opacity_ *= static_cast<float>(opacity) / 255.0f;   // (no offscreen target: nothing is isolated, so the bounds clip nothing)
  return opacity == 255;
}

void DeviceRenderer2D::pop_layer() {
  if (layer_overflow_) {
    --layer_overflow_;
    return;
  }
  if (!layer_depth_) return;
  --layer_depth_;
  if (layer_is_group_ & (1u << layer_depth_)) {
    layer_is_group_ &= ~(1u << layer_depth_);
    unwind_masks();   // (clips left open inside the layer)
    if (group_depth_) end_group(Matrix2D(), groups_[group_depth_ - 1].layer_opacity, ImageFilter::Nearest);
    return;
  }
  while (group_depth_ && groups_[group_depth_ - 1].mask && groups_[group_depth_ - 1].clip_depth >= layer_clip_depth_[layer_depth_]) finish_group(Matrix2D(), 255, ImageFilter::Nearest);
  opacity_ = layer_opacity_[layer_depth_];
  clip_depth_ = layer_clip_depth_[layer_depth_];  // clips pushed inside the layer end with it
  clip_overflow_ = layer_clip_overflow_[layer_depth_];
  apply_clip();
}

bool DeviceRenderer2D::begin_group(const Rect2D& bounds) { return open_group(bounds, false); }

bool DeviceRenderer2D::begin_filter_group(const Rect2D& bounds) { return filters_ok_ && open_group(bounds, true); }

bool DeviceRenderer2D::open_group(const Rect2D& bounds, bool detached) {
  if (!valid_ || !groups_ok_ || group_depth_ >= kMaxGroups || opacity_ != 1.0f || clip_depth_ >= kMaxClipDepth) return false;
  const Rect2D b = detached ? bounds : raster::intersect(bounds, clip_);
  if (!b.width || !b.height || uint64_t{b.width} * b.height > (uint64_t{1} << 24)) return false;
  if (!submit_pending()) return false;   // (what was drawn so far goes to the parent first)
  IImage* image = device_.create_image({{b.width, b.height}, Format::B8G8R8A8Unorm, kImageColorAttachment | kImageSampled});
  if (!image) return false;
  commands_->begin();
  RenderPassBegin pass;   // (clears to transparent black)
  pass.color = image;
  pass.clear_color_value = Vec4{0, 0, 0, 0};
  commands_->begin_render_pass(pass);
  commands_->end_render_pass();
  if (!commands_->end() || !device_.submit(commands_)) {
    device_.destroy_image(image);
    return false;
  }
  Group& g = groups_[group_depth_++];
  g.image = image;
  g.bounds = b;
  g.clip_depth = clip_depth_;
  g.clip_overflow = clip_overflow_;
  g.opacity = opacity_;
  g.layer = false;
  g.layer_opacity = 255;
  g.mask = nullptr;
  g.mask_slot = -1;
  g.detached = detached;
  g.prev_detach = detach_index_;
  clip_stack_[clip_depth_++] = b;   // (nothing outside the picture)
  if (detached) {
    detach_index_ = clip_depth_ - 1;
    clip_overflow_ = 0;
  }
  apply_clip();
  return true;
}

void DeviceRenderer2D::unwind_masks() {
  while (group_depth_ && groups_[group_depth_ - 1].mask) finish_group(Matrix2D(), 255, ImageFilter::Nearest);
}

bool DeviceRenderer2D::end_group(const Matrix2D& m, uint8_t opacity, ImageFilter filter) {
  unwind_masks();   // (clips left open inside the group end with it)
  if (!group_depth_) return false;
  return finish_group(m, opacity, filter);
}

bool DeviceRenderer2D::finish_group(const Matrix2D& m, uint8_t opacity, ImageFilter filter) {
  bool ok = submit_pending();   // the group's own shapes into its image
  const Group g = groups_[--group_depth_];
  clip_depth_ = g.clip_depth;   // clips pushed inside the group end with it
  clip_overflow_ = g.clip_overflow;
  detach_index_ = g.prev_detach;
  opacity_ = g.opacity;
  apply_clip();
  const float lx[4] = {0, static_cast<float>(g.bounds.width), static_cast<float>(g.bounds.width), 0}, ly[4] = {0, 0, static_cast<float>(g.bounds.height), static_cast<float>(g.bounds.height)};
  float xs[4], ys[4];
  for (int i = 0; i < 4; ++i) {
    const float x = static_cast<float>(g.bounds.x) + lx[i], y = static_cast<float>(g.bounds.y) + ly[i];
    xs[i] = m.a * x + m.c * y + m.e;
    ys[i] = m.b * x + m.d * y + m.f;
  }
  const float scale = static_cast<float>(opacity) / 255.0f;
  ok = ok && draw_textured_corners(*g.image, xs, ys, 0.0f, 0.0f, 1.0f, 1.0f, filter == ImageFilter::Nearest ? Filter::Nearest : Filter::Linear, &scale, g.mask);
  if (g.mask && g.mask_slot < 0) device_.destroy_image(g.mask);
  else if (g.mask && mask_cache_[g.mask_slot].in_use) --mask_cache_[g.mask_slot].in_use;
  device_.destroy_image(g.image);
  return ok;
}

int32_t DeviceRenderer2D::find_mask(uint64_t key, uint32_t w, uint32_t h) {
  for (uint32_t i = 0; i < kMaskCacheEntries; ++i) {
    MaskEntry& e = mask_cache_[i];
    if (e.image && e.key == key && e.width == w && e.height == h) {
      e.used = ++mask_clock_;
      return static_cast<int32_t>(i);
    }
  }
  return -1;
}

int32_t DeviceRenderer2D::keep_mask(uint64_t key, IImage* image, uint32_t w, uint32_t h) {
  const uint64_t pixels = uint64_t{w} * h;
  if (pixels > kMaskCachePixels) return -1;
  for (;;) {   // make room: the least recently used entry nobody is drawing through goes first
    int32_t free_slot = -1, oldest = -1;
    for (uint32_t i = 0; i < kMaskCacheEntries; ++i) {
      const MaskEntry& e = mask_cache_[i];
      if (!e.image) {
        if (free_slot < 0) free_slot = static_cast<int32_t>(i);
      } else if (!e.in_use && (oldest < 0 || e.used < mask_cache_[oldest].used)) {
        oldest = static_cast<int32_t>(i);
      }
    }
    if (free_slot >= 0 && mask_cache_pixels_ + pixels <= kMaskCachePixels) {
      MaskEntry& e = mask_cache_[free_slot];
      e.key = key;
      e.width = w;
      e.height = h;
      e.image = image;
      e.in_use = 0;
      e.used = ++mask_clock_;
      mask_cache_pixels_ += pixels;
      return free_slot;
    }
    if (oldest < 0) return -1;   // everything is in use
    MaskEntry& e = mask_cache_[oldest];
    device_.destroy_image(e.image);
    mask_cache_pixels_ -= uint64_t{e.width} * e.height;
    e.image = nullptr;
  }
}

// FNV-1a over 32-bit words.
static uint64_t mix(uint64_t h, uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    h ^= (v >> (8 * i)) & 0xFF;
    h *= 1099511628211ull;
  }
  return h;
}
static uint32_t float_bits(float f) {
  uint32_t v = 0;
  const uint8_t* src = reinterpret_cast<const uint8_t*>(&f);   // (a float's bytes, copied out: same size, no alignment question)
  uint8_t* dst = reinterpret_cast<uint8_t*>(&v);
  for (int i = 0; i < 4; ++i) dst[i] = src[i];
  return v;
}

// A clip that is not a rectangle, exact: what is drawn under it goes to a group of the clip's bounds, laid over the parent through the clip's coverage at pop_clip().
bool DeviceRenderer2D::push_exact_clip(const Rect2D& rect, const raster::RRect* rounded, const MaskView* mask) {
  if (!valid_ || !masks_ok_ || clip_overflow_ || clip_depth_ >= kMaxClipDepth || group_depth_ >= kMaxGroups || opacity_ != 1.0f) return false;
  const Rect2D b = raster::intersect(rect, clip_);
  if (!b.width || !b.height) {   // nothing of it is on the target: nothing shows
    clip_stack_[clip_depth_++] = {0, 0, 0, 0};
    apply_clip();
    return true;
  }
  const uint64_t count = uint64_t{b.width} * b.height;
  if (count > (uint64_t{1} << 24)) return false;
  // what the coverage of this part of the clip depends on: the shape relative to the part, or the mask's bytes
  uint64_t key = 14695981039346656037ull;
  key = mix(key, rounded ? 1u : 2u);
  if (rounded) {
    key = mix(mix(mix(mix(key, static_cast<uint32_t>(rounded->x0 - b.x)), static_cast<uint32_t>(rounded->y0 - b.y)), static_cast<uint32_t>(rounded->x1 - b.x)), static_cast<uint32_t>(rounded->y1 - b.y));
    for (int i = 0; i < 4; ++i) key = mix(mix(key, float_bits(rounded->rx[i])), float_bits(rounded->ry[i]));
  } else {
    for (uint32_t j = 0; j < b.height; ++j) {
      const uint64_t row = uint64_t{static_cast<uint32_t>(b.y - rect.y) + j} * mask->pitch + static_cast<uint32_t>(b.x - rect.x);
      for (uint32_t i = 0; i < b.width; ++i) key = (key ^ mask->coverage[row + i]) * 1099511628211ull;
    }
  }
  int32_t slot = find_mask(key, b.width, b.height);
  IImage* image = slot >= 0 ? mask_cache_[slot].image : nullptr;
  if (slot >= 0) {
    ++mask_hits_;
  } else {
    ++mask_misses_;
    if (!prepare_rows(count)) return false;
    const ArrayView<uint32_t> rows = texture_rows_.first(count);
    for (uint32_t j = 0; j < b.height; ++j) {
      const int32_t y = b.y + static_cast<int32_t>(j);
      for (uint32_t i = 0; i < b.width; ++i) {
        const int32_t x = b.x + static_cast<int32_t>(i);
        uint32_t cov = 0;
        if (rounded) cov = rounded->coverage(x, y);
        else cov = mask->coverage[uint64_t{static_cast<uint32_t>(y - rect.y)} * mask->pitch + static_cast<uint32_t>(x - rect.x)];
        rows[uint64_t{j} * b.width + i] = cov << 24;
      }
    }
    image = device_.create_image({{b.width, b.height}, Format::B8G8R8A8Unorm, kImageSampled});
    if (!image) return false;
    if (!image->write_pixels(ArrayView<const uint32_t>(texture_rows_.data(), count))) {
      device_.destroy_image(image);
      return false;
    }
    slot = keep_mask(key, image, b.width, b.height);
  }
  if (!open_group(b, false)) {
    if (slot < 0) device_.destroy_image(image);   // (a cached one stays for next time)
    return false;
  }
  Group& g = groups_[group_depth_ - 1];
  g.mask = image;
  g.mask_slot = slot;
  if (slot >= 0) ++mask_cache_[slot].in_use;
  return true;
}

bool DeviceRenderer2D::pass_to(IImage* dest, uint32_t w, uint32_t h, IImage* source, IPipeline* pipeline, const void* constants, uint32_t size, Filter filter, bool clear) {
  const float fw = static_cast<float>(w), fh = static_cast<float>(h);
  const TexturedVertex tl{{0, 0, 0}, {0, 0}}, tr{{fw, 0, 0}, {1, 0}}, br{{fw, fh, 0}, {1, 1}}, bl{{0, fh, 0}, {0, 1}};
  const TexturedVertex quad[6] = {tl, tr, br, tl, br, bl};
  const ArrayView<TexturedVertex> mapped = texture_vertices_->map().cast<TexturedVertex>();
  if (mapped.size() < 6) return false;
  mapped.copy_from(ArrayView<const TexturedVertex>(quad, 6));
  texture_vertices_->unmap();
  commands_->begin();
  RenderPassBegin pass;
  pass.color = dest;
  pass.clear_color = clear;
  pass.clear_color_value = Vec4{0, 0, 0, 0};
  commands_->begin_render_pass(pass);
  commands_->bind_pipeline(pipeline);
  commands_->bind_vertex_buffer(texture_vertices_);
  SamplerDesc sampler;
  sampler.filter = filter;
  sampler.address_u = sampler.address_v = AddressMode::ClampToEdge;
  commands_->bind_texture(0, source, sampler);
  commands_->push_constants(constants, size);
  commands_->set_scissor({0, 0, w, h});
  commands_->draw(6);
  commands_->end_render_pass();
  return commands_->end() && device_.submit(commands_);
}

// The top group's picture through `pipeline` (one textured rectangle over a new image of the same size, sampled linearly): the result replaces the picture.
bool DeviceRenderer2D::filter_pass(IPipeline* pipeline, void* constants, uint32_t size, bool) {
  Group& g = groups_[group_depth_ - 1];
  const uint32_t w = g.bounds.width, h = g.bounds.height;
  IImage* dest = device_.create_image({{w, h}, Format::B8G8R8A8Unorm, kImageColorAttachment | kImageSampled});
  if (!dest) return false;
  if (!pass_to(dest, w, h, g.image, pipeline, constants, size, Filter::Linear, true)) {
    device_.destroy_image(dest);
    return false;
  }
  device_.destroy_image(g.image);
  g.image = dest;
  return true;
}

static Mat4 image_matrix(uint32_t w, uint32_t h) {
  Mat4 m;
  m.m[0] = 2.0f / static_cast<float>(w);
  m.m[5] = 2.0f / static_cast<float>(h);
  m.m[10] = 0;
  m.m[12] = -1;
  m.m[13] = -1;
  m.m[14] = 0.5f;
  return m;
}

// One box blur pass of `radius` texels along a row or a column. Up to radius 8 the 17 taps are on whole texels (and the weights cut off at the radius); beyond it they are
// spread over the radius, so the box is sampled sparsely (the linear filter fills in between).
bool DeviceRenderer2D::blur_pass(int32_t radius, bool horizontal) {
  const Group& g = groups_[group_depth_ - 1];
  TexturedBlurConstants c;
  c.mvp = image_matrix(g.bounds.width, g.bounds.height);
  for (float& w : c.weight) w = 0.0f;
  const float spacing = radius > 8 ? static_cast<float>(radius) / 8.0f : 1.0f;
  const int32_t reach = radius > 8 ? 8 : radius;   // taps on each side that count
  const float each = 1.0f / static_cast<float>(2 * reach + 1);
  for (int32_t i = 0; i <= reach; ++i) c.weight[i] = each;
  c.step[0] = horizontal ? spacing / static_cast<float>(g.bounds.width) : 0.0f;
  c.step[1] = horizontal ? 0.0f : spacing / static_cast<float>(g.bounds.height);
  c.pad[0] = c.pad[1] = 0;
  return filter_pass(filter_blur_, &c, sizeof(c), horizontal);
}

bool DeviceRenderer2D::group_blur(float sigma) {
  if (!group_depth_ || !filters_ok_) return false;
  if (!(sigma >= 0.3f)) return true;   // (nothing to blur)
  if (sigma > 500.0f) sigma = 500.0f;
  unwind_masks();
  if (!group_depth_ || !submit_pending()) return false;
  // three box sizes (odd) whose blurs in a row approximate a Gaussian of that standard deviation (the same as the CPU blur)
  const float s2 = sigma * sigma;
  const float ideal = raster::sqrt_f(12.0f * s2 / 3.0f + 1.0f);
  int32_t wl = static_cast<int32_t>(ideal);
  if (wl % 2 == 0) --wl;
  if (wl < 1) wl = 1;
  const int32_t wu = wl + 2;
  const float m_ideal = (12.0f * s2 - 3.0f * static_cast<float>(wl * wl) - 12.0f * static_cast<float>(wl) - 9.0f) / (-4.0f * static_cast<float>(wl) - 4.0f);
  const int32_t m = static_cast<int32_t>(m_ideal + 0.5f);
  for (int i = 0; i < 3; ++i) {
    const int32_t radius = ((i < m ? wl : wu) - 1) / 2;
    if (radius <= 0) continue;
    if (!blur_pass(radius, true) || !blur_pass(radius, false)) return false;
  }
  return true;
}

bool DeviceRenderer2D::group_drop_shadow(int32_t dx, int32_t dy, float sigma, uint32_t color) {
  unwind_masks();
  if (!group_depth_ || !filters_ok_ || !submit_pending()) return false;
  Group& g = groups_[group_depth_ - 1];
  const uint32_t w = g.bounds.width, h = g.bounds.height;
  const ImageDesc desc{{w, h}, Format::B8G8R8A8Unorm, kImageColorAttachment | kImageSampled};
  IImage* shadow = device_.create_image(desc);
  IImage* result = device_.create_image(desc);
  bool ok = shadow && result;
  // 1. the picture's alpha, moved and tinted
  if (ok) {
    TexturedShadowConstants c;
    c.mvp = image_matrix(w, h);
    c.color[0] = static_cast<float>((color >> 16) & 0xFF) / 255.0f;
    c.color[1] = static_cast<float>((color >> 8) & 0xFF) / 255.0f;
    c.color[2] = static_cast<float>(color & 0xFF) / 255.0f;
    c.color[3] = static_cast<float>(color >> 24) / 255.0f;
    c.offset[0] = static_cast<float>(dx) / static_cast<float>(w);
    c.offset[1] = static_cast<float>(dy) / static_cast<float>(h);
    c.pad[0] = c.pad[1] = 0;
    ok = pass_to(shadow, w, h, g.image, filter_shadow_, &c, sizeof(c), Filter::Nearest, true);
  }
  // 2. blurred (the blur works on the top group's picture: it stands in for the group's for that)
  IImage* content = g.image;
  if (ok) {
    g.image = shadow;
    ok = group_blur(sigma);
    shadow = g.image;
    g.image = content;
  }
  // 3. the shadow, then the picture over it
  if (ok) {
    TexturedScaledConstants c;
    c.mvp = image_matrix(w, h);
    c.scale = 1.0f;
    c.pad[0] = c.pad[1] = c.pad[2] = 0;
    ok = pass_to(result, w, h, shadow, composite_, &c, sizeof(c), Filter::Nearest, true) && pass_to(result, w, h, content, composite_, &c, sizeof(c), Filter::Nearest, false);
  }
  if (shadow) device_.destroy_image(shadow);
  if (ok) {
    device_.destroy_image(content);
    g.image = result;
  } else if (result) {
    device_.destroy_image(result);
  }
  return ok;
}

bool DeviceRenderer2D::group_cut_out(const Rect2D& rect, const CornerRadii& radii) {
  unwind_masks();
  if (!group_depth_ || !filters_ok_ || erase_) return false;
  close_range();   // (what was recorded before is not erased)
  erase_ = true;
  fill_rounded_rect(rect, radii, 0xFF000000u);
  close_range();
  erase_ = false;
  return true;
}

bool DeviceRenderer2D::group_color(const ColorTransform& t) {
  unwind_masks();
  if (!group_depth_ || !filters_ok_) return false;
  if (!submit_pending()) return false;
  const Group& g = groups_[group_depth_ - 1];
  TexturedColorConstants c;
  c.mvp = image_matrix(g.bounds.width, g.bounds.height);
  for (int r = 0; r < 3; ++r) {
    for (int k = 0; k < 3; ++k) c.row[r][k] = t.m[r * 3 + k];
    c.row[r][3] = t.add[r];
  }
  c.alpha_scale = t.alpha_scale;
  c.pad[0] = c.pad[1] = c.pad[2] = 0;
  return filter_pass(filter_color_, &c, sizeof(c), true);
}

bool DeviceRenderer2D::copy_rect(const Rect2D& source, int32_t dest_x, int32_t dest_y) {
  if (!valid_ || group_depth_) return false;
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
  reset_sheet();
}

bool DeviceRenderer2D::read_pixels(ArrayView<uint32_t> out) { return submit_pending() && images_[back_] && images_[back_]->read_pixels(out); }

}  // namespace graphics
