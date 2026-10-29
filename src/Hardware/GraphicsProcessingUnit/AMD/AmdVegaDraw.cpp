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

#include "Hardware/GraphicsProcessingUnit/AMD/AmdVegaDraw.hpp"
#include "Utility/ByteStream.hpp"

namespace hardware {

using namespace gcn;

namespace {

uint64_t align_to(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }
uint64_t align_up(uint64_t v) { return align_to(v, AmdVegaDraw::kAlignment); }

struct Plan {
  uint64_t vertex_code, pixel_code, vertex_constants, pixel_constants, table, vertices, indices, target, depth, stencil, textures[abi::kMaxSamplers], total, alignment;
};

bool layout_of(uint32_t tile_index, TileLayout* layout) { return resolve_tile_index(tile_index, layout); }

bool bound(const VegaTexture& t) { return t.texels.size() != 0; }

// The image the texture is, at `address`
ImageDescriptor image_of(const VegaTexture& t, uint64_t address) {
  TileLayout layout;
  ImageDescriptor image;
  image.address = address;
  image.width = t.width;
  image.height = t.height;
  image.format = t.format;
  image.tile_index = t.tile_index;
  image.type = t.type;
  image.depth = t.depth;
  image.levels = t.levels;
  const bool chain = !(t.levels == 1 && t.depth == 1 && (t.type == ImageType::Image2D || t.type == ImageType::Image1D));
  image.pitch_pixels = chain ? image_pitch(t.width, t.height, t.levels, texel_bytes(t.format), t.tile_index)
                             : (layout_of(t.tile_index, &layout) ? static_cast<uint32_t>(align_to(t.width, surface_pitch_align(layout, texel_bytes(t.format)))) : 0);
  return image;
}

// The texels the request must hold for the texture: every level, every slice, tightly packed
uint64_t texture_texels(const VegaTexture& t) {
  uint64_t n = 0;
  for (uint32_t k = 0; k < t.levels && k < 32; ++k) {
    const uint64_t w = (t.width >> k) > 0 ? t.width >> k : 1, h = (t.height >> k) > 0 ? t.height >> k : 1;
    const uint64_t d = t.type == ImageType::Image3D ? ((t.depth >> k) > 0 ? t.depth >> k : 1) : t.depth;
    n += w * h * d;
  }
  return n;
}

// Whether the texture can be described and its texels are the size they should be.
bool texture_ok(const VegaTexture& t) {
  uint32_t w[8];
  return make_image_descriptor(ArrayView<uint32_t>(w, 8), image_of(t, 0)) && t.texels.size() == texture_texels(t) * texel_bytes(t.format);
}

// constants the shaders read are padded to the 64 rows the command processor model probes for, and to at least what the shaders say
constexpr uint64_t kConstantBytes = TgsiShader::kMaxConsts * 16;

bool make_plan(const VegaDrawRequest& r, Plan* p) {
  if (r.width == 0 || r.height == 0 || r.width > 16384 || r.height > 16384 || r.vertex_code.size() == 0 || r.pixel_code.size() == 0 || r.vertex_data.size() == 0 || r.vertex_stride == 0) return false;
  if (r.vertex_info.const_rows * 4ull > r.vertex_constants.size() || r.pixel_info.const_rows * 4ull > r.pixel_constants.size() || r.vertex_constants.size() > kConstantBytes / 4 ||
      r.pixel_constants.size() > kConstantBytes / 4)
    return false;
  TileLayout layout;
  if (!layout_of(r.target_tile_index, &layout) || AmdVegaDraw::target_pitch(r) == 0) return false;
  if (AmdVegaDraw::uses_depth(r) || AmdVegaDraw::uses_stencil(r)) {
    if (AmdVegaDraw::plane_pitch(r) == 0) return false;
  }
  for (const VegaTexture& t : r.textures)
    if (bound(t) && !texture_ok(t)) return false;
  uint64_t at = 0;
  // (a piece starts at a multiple of the 256 bytes and of the alignment its layout wants)
  p->alignment = AmdVegaDraw::kAlignment;
  auto place = [&](uint64_t bytes, uint64_t alignment = AmdVegaDraw::kAlignment) { if (alignment > p->alignment) p->alignment = alignment; at = align_to(at, alignment); const uint64_t start = at; at = align_up(at + bytes); return start; };
  const uint64_t color_align = surface_base_align(layout, ColorTarget::kPixelBytes);
  p->vertex_code = place(r.vertex_code.size() * 4ull);
  p->pixel_code = place(r.pixel_code.size() * 4ull);
  p->vertex_constants = place(kConstantBytes);
  p->pixel_constants = place(kConstantBytes);
  p->table = place(abi::kTableBytes);
  p->vertices = place(r.vertex_data.size() * 4ull);
  p->indices = place(r.indices.size() == 0 ? 4 : r.indices.size() * 4ull);
  p->target = place(AmdVegaDraw::target_bytes(r), color_align);
  p->depth = p->stencil = 0;
  TileLayout z_layout, s_layout;
  if (AmdVegaDraw::uses_depth(r)) {
    layout_of(r.depth_tile_index, &z_layout);
    p->depth = place(AmdVegaDraw::depth_bytes(r), surface_base_align(z_layout, DepthTarget::kZBytes));
  }
  if (AmdVegaDraw::uses_stencil(r)) {
    layout_of(r.stencil_tile_index, &s_layout);
    p->stencil = place(AmdVegaDraw::stencil_bytes(r), surface_base_align(s_layout, DepthTarget::kStencilBytes));
  }
  for (uint32_t i = 0; i < abi::kMaxSamplers; ++i) {
    p->textures[i] = 0;
    if (!bound(r.textures[i])) continue;
    TileLayout tl;
    layout_of(r.textures[i].tile_index, &tl);
    p->textures[i] = place(image_bytes(image_of(r.textures[i], 0)), image_base_align(image_of(r.textures[i], 0)));
  }
  p->total = at;
  return at <= (1ull << 31);
}

}  // namespace

uint32_t AmdVegaDraw::target_pitch(const VegaDrawRequest& r) {
  TileLayout layout;
  if (!layout_of(r.target_tile_index, &layout)) return 0;
  const uint32_t pitch = static_cast<uint32_t>(align_to(pitch_pixels(r.width), surface_pitch_align(layout, ColorTarget::kPixelBytes)));
  return surface_valid(layout, ColorTarget::kPixelBytes, pitch) ? pitch : 0;
}

uint32_t AmdVegaDraw::plane_pitch(const VegaDrawRequest& r) {
  TileLayout z, s;
  uint64_t align = 8;
  if (uses_depth(r)) {
    if (!layout_of(r.depth_tile_index, &z)) return 0;
    align = align < surface_pitch_align(z, DepthTarget::kZBytes) ? surface_pitch_align(z, DepthTarget::kZBytes) : align;
  }
  if (uses_stencil(r)) {
    if (!layout_of(r.stencil_tile_index, &s)) return 0;
    align = align < surface_pitch_align(s, DepthTarget::kStencilBytes) ? surface_pitch_align(s, DepthTarget::kStencilBytes) : align;
  }
  const uint32_t pitch = static_cast<uint32_t>(align_to(r.width, align));
  return (!uses_depth(r) || surface_valid(z, DepthTarget::kZBytes, pitch)) && (!uses_stencil(r) || surface_valid(s, DepthTarget::kStencilBytes, pitch)) ? pitch : 0;
}

uint64_t AmdVegaDraw::target_bytes(const VegaDrawRequest& r) {
  TileLayout layout;
  const uint32_t pitch = target_pitch(r);
  if (pitch == 0 || !layout_of(r.target_tile_index, &layout)) return 0;
  return layout.kind == TileLayout::Kind::Linear ? static_cast<uint64_t>(pitch) * r.height * ColorTarget::kPixelBytes : surface_bytes(layout, ColorTarget::kPixelBytes, pitch, r.height);
}

uint64_t AmdVegaDraw::depth_bytes(const VegaDrawRequest& r) {
  TileLayout layout;
  return uses_depth(r) && plane_pitch(r) != 0 && layout_of(r.depth_tile_index, &layout) ? surface_bytes(layout, DepthTarget::kZBytes, plane_pitch(r), r.height) : 0;
}

uint64_t AmdVegaDraw::stencil_bytes(const VegaDrawRequest& r) {
  TileLayout layout;
  return uses_stencil(r) && plane_pitch(r) != 0 && layout_of(r.stencil_tile_index, &layout) ? surface_bytes(layout, DepthTarget::kStencilBytes, plane_pitch(r), r.height) : 0;
}

uint64_t AmdVegaDraw::pixel_offset(const VegaDrawRequest& r, uint32_t x, uint32_t y) {
  TileLayout layout;
  return layout_of(r.target_tile_index, &layout) ? surface_offset(layout, ColorTarget::kPixelBytes, target_pitch(r), x, y, r.height) : 0;
}

uint64_t AmdVegaDraw::depth_offset(const VegaDrawRequest& r, uint32_t x, uint32_t y) {
  TileLayout layout;
  return layout_of(r.depth_tile_index, &layout) ? surface_offset(layout, DepthTarget::kZBytes, plane_pitch(r), x, y, r.height) : 0;
}

uint64_t AmdVegaDraw::stencil_offset(const VegaDrawRequest& r, uint32_t x, uint32_t y) {
  TileLayout layout;
  return layout_of(r.stencil_tile_index, &layout) ? surface_offset(layout, DepthTarget::kStencilBytes, plane_pitch(r), x, y, r.height) : 0;
}

uint64_t AmdVegaDraw::area_alignment(const VegaDrawRequest& r) {
  Plan p;
  return make_plan(r, &p) ? p.alignment : 0;
}

uint64_t AmdVegaDraw::area_bytes(const VegaDrawRequest& r) {
  Plan p;
  return make_plan(r, &p) ? p.total : 0;
}

bool AmdVegaDraw::draw(AmdVegaM& card, const VegaDrawRequest& r, uint64_t area_offset, ByteView staging, ByteView pixels, ByteView depth, ByteView stencil) {
  Plan p;
  // (the card places a surface by its address: the area must start where the most demanding layout wants it)
  if (!card.up() || !make_plan(r, &p) || area_offset % p.alignment != 0 || card.framebuffer_gpu_address() % p.alignment != 0) return false;
  if (staging.size() < p.total || pixels.size() < target_bytes(r) || (depth.size() != 0 && depth.size() < depth_bytes(r)) || (stencil.size() != 0 && stencil.size() < stencil_bytes(r)) || area_offset > card.layout().framebuffer_bytes || p.total > card.layout().framebuffer_bytes - area_offset) return false;
  const uint32_t vertex_count = r.indices.size() != 0 ? static_cast<uint32_t>(r.indices.size()) : r.vertex_count;
  if (vertex_count == 0 || vertex_count % 3 != 0 || r.indices.size() > 0x10000000u) return false;
  const uint64_t base = card.framebuffer_gpu_address() + area_offset;

  // the staging image of the area
  ByteWriter w(staging.first(p.total));
  w.zeros(p.total);
  auto put_words = [&](uint64_t offset, ArrayView<const uint32_t> words) {
    for (uint64_t i = 0; i < words.size(); ++i) {
      const uint32_t v = words[i];
      const uint8_t b[4] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 24)};
      w.patch_bytes(offset + 4 * i, ConstByteView(b, 4));
    }
  };
  auto float_word = [](float f) { union { float f; uint32_t u; } c; c.f = f; return c.u; };
  put_words(p.vertex_code, r.vertex_code);
  put_words(p.pixel_code, r.pixel_code);
  for (uint64_t i = 0; i < r.vertex_constants.size(); ++i) { const uint32_t v = float_word(r.vertex_constants[i]); put_words(p.vertex_constants + 4 * i, ArrayView<const uint32_t>(&v, 1)); }
  for (uint64_t i = 0; i < r.pixel_constants.size(); ++i) { const uint32_t v = float_word(r.pixel_constants[i]); put_words(p.pixel_constants + 4 * i, ArrayView<const uint32_t>(&v, 1)); }
  const uint64_t vertex_records = static_cast<uint64_t>(r.vertex_data.size()) * 4 / r.vertex_stride;
  for (uint32_t a = 0; a < abi::kMaxAttributes; ++a) {
    if (!(r.vertex_info.attributes_used >> a & 1)) continue;
    const VertexAttribute& at = r.attributes[a];
    uint32_t d[4];
    if (at.components < 1 || at.components > 4 || at.offset > r.vertex_stride || r.vertex_stride - at.offset < at.components * 4u || vertex_records == 0 || vertex_records > 0xFFFFFFFFu ||
        !make_vertex_descriptor(ArrayView<uint32_t>(d, 4), base + p.vertices + at.offset, r.vertex_stride, static_cast<uint32_t>(vertex_records), at.components))
      return false;
    put_words(p.table + a * abi::kVertexBufferBytes, ArrayView<const uint32_t>(d, 4));
  }
  put_words(p.vertices, r.vertex_data);
  if (r.indices.size() != 0) put_words(p.indices, r.indices);
  // the colour buffer (all of it, the padding too) and the planes hold the clear values, the same whatever the layout
  for (uint64_t i = 0; i < target_bytes(r) / ColorTarget::kPixelBytes; ++i)
    for (uint32_t c = 0; c < 4; ++c) { const uint32_t v = float_word(r.clear[c]); put_words(p.target + (i * 4 + c) * 4, ArrayView<const uint32_t>(&v, 1)); }
  for (uint64_t i = 0; i < depth_bytes(r) / DepthTarget::kZBytes; ++i) { const uint32_t v = float_word(r.clear_depth); put_words(p.depth + i * 4, ArrayView<const uint32_t>(&v, 1)); }
  for (uint64_t i = 0; i < stencil_bytes(r); ++i) { const uint8_t b = r.clear_stencil; w.patch_bytes(p.stencil + i, ConstByteView(&b, 1)); }
  // the textures in their layout, and their descriptors
  for (uint32_t i = 0; i < abi::kMaxSamplers; ++i) {
    const VegaTexture& t = r.textures[i];
    if (!bound(t)) continue;
    const uint32_t texel = texel_bytes(t.format);
    const ImageDescriptor image = image_of(t, base + p.textures[i]);
    if (!texture_ok(t)) return false;
    uint64_t next = 0;
    for (uint32_t k = 0; k < t.levels; ++k) {
      LevelLayout level;
      if (!image_level(image, k, &level)) return false;
      for (uint32_t z = 0; z < level.depth; ++z)
        for (uint32_t y = 0; y < level.height; ++y)
          for (uint32_t x = 0; x < level.width; ++x, ++next) w.patch_bytes(p.textures[i] + image_texel_offset(image, level, x, y, z), t.texels.subview(next * texel, texel));
    }
    uint32_t iw[8], sw[4];
    if (!make_image_descriptor(ArrayView<uint32_t>(iw, 8), image) || !make_sampler_descriptor(ArrayView<uint32_t>(sw, 4), t.sampler)) return false;
    put_words(p.table + abi::kImageOffset + i * abi::kImageBytes, ArrayView<const uint32_t>(iw, 8));
    put_words(p.table + abi::kSamplerOffset + i * abi::kSamplerBytes, ArrayView<const uint32_t>(sw, 4));
  }
  if (!w.ok() || !card.upload(ConstByteView(staging.data(), p.total), area_offset)) return false;

  // the stream
  DrawState s;
  s.vertex.address = base + p.vertex_code;
  s.vertex.info = r.vertex_info;
  s.pixel.address = base + p.pixel_code;
  s.pixel.info = r.pixel_info;
  s.vertex_constants = base + p.vertex_constants;
  s.pixel_constants = base + p.pixel_constants;
  s.descriptor_table = base + p.table;
  s.target.address = base + p.target;
  s.target.width = r.width;
  s.target.height = r.height;
  s.target.pitch_pixels = target_pitch(r);
  s.target.tile_index = r.target_tile_index;
  s.blend = r.blend;
  s.depth_stencil = r.depth_stencil;
  s.depth.pitch_pixels = plane_pitch(r);
  s.depth.z_tile_index = r.depth_tile_index;
  s.depth.stencil_tile_index = r.stencil_tile_index;
  if (uses_depth(r)) s.depth.z_address = base + p.depth;
  if (uses_stencil(r)) s.depth.stencil_address = base + p.stencil;
  for (uint32_t k = 0; k < 16; ++k) s.ps_param[k] = r.ps_param[k];
  s.index_count = vertex_count;
  if (r.indices.size() != 0) {
    s.index_address = base + p.indices;
    s.index_max = vertex_count;
  }
  uint32_t stream[AmdVegaM::kIbDwords / 2];
  Pm4Writer out(ArrayView<uint32_t>(stream, AmdVegaM::kIbDwords / 2));
  if (!emit_draw(out, s) || !out.ok()) return false;
  if (!card.submit_graphics(out.words())) return false;
  if (!card.download(ByteView(pixels.data(), target_bytes(r)), area_offset + p.target)) return false;
  if (depth.size() != 0 && uses_depth(r) && !card.download(ByteView(depth.data(), depth_bytes(r)), area_offset + p.depth)) return false;
  if (stencil.size() != 0 && uses_stencil(r) && !card.download(ByteView(stencil.data(), stencil_bytes(r)), area_offset + p.stencil)) return false;
  return true;
}

}  // namespace hardware
