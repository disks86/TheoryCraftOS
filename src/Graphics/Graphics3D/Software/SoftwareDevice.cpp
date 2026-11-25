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

#include "Graphics/Graphics3D/Software/SoftwareDevice.hpp"
#include <new>
#include "Graphics/Graphics2D/Software/SoftwareRenderer2D.hpp"
#include "Graphics/Graphics3D/Software/Tessellator.hpp"
#include "Graphics/Graphics3D/VertexFetch.hpp"
#include "Graphics/BlockCompression.hpp"
#include "Graphics/WidePixels.hpp"
#include "Utility/AllocatedView.hpp"

namespace graphics {

namespace {

template <class T, class... Args>
T* make(allocators::IAllocator& allocator, Args&&... args) {
  void* memory = allocator.allocate(sizeof(T));
  return memory ? new (memory) T(static_cast<Args&&>(args)...) : nullptr;
}

template <class T>
void destroy(allocators::IAllocator& allocator, T* object) {
  if (!object) return;
  object->~T();
  allocator.deallocate(object);
}

// The tessellator of a draw with patches (made only when there are some), freed when the draw ends.
class TessellatorHolder {
 public:
  TessellatorHolder(allocators::IAllocator& allocator, bool wanted) : allocator_(allocator), tessellator_(wanted ? make<Tessellator>(allocator, allocator) : nullptr) {
    if (tessellator_ && !tessellator_->valid()) {
      destroy(allocator_, tessellator_);
      tessellator_ = nullptr;
    }
  }
  ~TessellatorHolder() { destroy(allocator_, tessellator_); }
  TessellatorHolder(const TessellatorHolder&) = delete;
  TessellatorHolder& operator=(const TessellatorHolder&) = delete;
  bool valid() const { return tessellator_ != nullptr; }
  Tessellator* get() const { return tessellator_; }

 private:
  allocators::IAllocator& allocator_;
  Tessellator* tessellator_;
};

// --- Resources ---------------------------------------------------------------

// sRGB: the 256 linear values the 8-bit codes mean ((c + 0.055) / 1.055 raised to 2.4 above 0.04045, c / 12.92 below), worked out once without a maths library (the power as a square times a
// fifth root, found by Newton's iteration); and the nearest code of a linear value, by a search over that table.
const float* srgb_to_linear_table() {
  static float table[256];
  static bool ready = false;
  if (ready) return table;
  for (uint32_t i = 0; i < 256; ++i) {
    const double c = i / 255.0;
    double linear;
    if (c <= 0.04045) {
      linear = c / 12.92;
    } else {
      const double p = (c + 0.055) / 1.055, p2 = p * p;
      double r = p;   // the fifth root of p squared: p to the 0.4
      for (int k = 0; k < 40; ++k) {
        const double r4 = r * r * r * r;
        r = r - (r4 * r - p2) / (5.0 * r4);
      }
      linear = p2 * r;
    }
    table[i] = static_cast<float>(linear);
  }
  ready = true;
  return table;
}

uint32_t linear_to_srgb_code(float v) {
  const float* table = srgb_to_linear_table();
  if (!(v > 0.0f)) return 0;
  if (v >= 1.0f) return 255;
  uint32_t low = 0, high = 255;   // the code is in [low, high]; find the last one whose linear value is not above v, then the nearer of it and the next
  while (high - low > 1) {
    const uint32_t mid = (low + high) / 2;
    if (table[mid] <= v) low = mid;
    else high = mid;
  }
  return (v - table[low]) <= (table[low + 1] - v) ? low : low + 1;
}

// A linear colour as the 0xAARRGGBB of an sRGB target: the colour channels encoded, the alpha not.
uint32_t pack_srgb(Vec4 c) {
  const float a = !(c.w > 0.0f) ? 0.0f : c.w >= 1.0f ? 1.0f : c.w;
  return (static_cast<uint32_t>(a * 255.0f + 0.5f) << 24) | (linear_to_srgb_code(c.x) << 16) | (linear_to_srgb_code(c.y) << 8) | linear_to_srgb_code(c.z);
}

class SoftwareBuffer final : public IBuffer {
 public:
  SoftwareBuffer(allocators::IAllocator& allocator, uint64_t size) : allocator_(allocator) {
    data_ = allocate_view<uint8_t>(allocator, size);
    if (!data_.empty()) data_.fill(0);
  }
  ~SoftwareBuffer() { release_view(allocator_, data_); }
  bool valid() const { return !data_.empty(); }
  uint64_t size() const override { return data_.size(); }
  ByteView map() override { return data_; }
  void unmap() override {}

 private:
  allocators::IAllocator& allocator_;
  ByteView data_;
};

class SoftwareImage final : public IImage {
 public:
  // The storage holds every level one after the other, and in each level its layers (a volume's level: its slices) one after the other, each width * height * samples values (a
  // multisampled image keeps its samples one after the other for each pixel).
  SoftwareImage(allocators::IAllocator& allocator, const ImageDesc& desc) : allocator_(allocator), desc_(desc) {
    uint64_t total = 0;
    for (uint32_t level = 0; level < desc.mip_levels && level < kMaxMipLevels; ++level) {
      level_offset_[level] = total;
      total += level_pixels(level) * level_layers(level);
    }
    if (desc.format == Format::D32Float) {
      depth_ = allocate_view<float>(allocator, total);
      if (desc.stencil) stencil_ = allocate_view<uint8_t>(allocator, total);
    } else if (is_wide_format(desc.format)) {
      hdr_ = allocate_view<Vec4>(allocator, total);
    } else if (desc.format != Format::Undefined) {
      color_ = allocate_view<uint32_t>(allocator, total);
    }
    for (float& d : depth_) d = 1.0f;
    for (uint32_t& c : color_) c = 0;
    for (Vec4& c : hdr_) c = {0, 0, 0, 0};
    for (uint8_t& v : stencil_) v = 0;
  }
  ~SoftwareImage() {
    release_view(allocator_, color_);
    release_view(allocator_, hdr_);
    release_view(allocator_, depth_);
    release_view(allocator_, stencil_);
  }
  bool valid() const { return (!color_.empty() || !hdr_.empty() || !depth_.empty()) && (!desc_.stencil || !stencil_.empty()); }
  uint32_t mip_levels() const override { return desc_.mip_levels; }
  uint32_t samples() const { return desc_.samples; }
  // (the values of level 0: every sample of every pixel of its layers)
  ArrayView<uint32_t> all_samples() const { return color_.empty() ? color_ : color_.first(level_pixels(0) * level_layers(0)); }
  uint32_t layer_count() const override { return image_layer_count(desc_.kind, desc_.layers); }
  // The layers of level `level` (a volume's slices at that level).
  uint32_t level_layers(uint32_t level) const { return level_layer_count(desc_.kind, desc_.layers, level); }
  bool has(uint32_t level, uint32_t layer) const { return level < desc_.mip_levels && layer < level_layers(level); }
  Extent2D level_extent(uint32_t level) const { return {mip_extent(desc_.extent.width, level), mip_extent(desc_.extent.height, level)}; }
  TextureKind kind() const { return desc_.kind; }

  // --- what the memory manager fills and reads
  bool write_subresource(uint32_t level, uint32_t layer, ArrayView<const uint32_t> pixels) override {
    if (color_.empty() || desc_.samples != 1 || !has(level, layer)) return false;
    const Surface s = surface(level, layer);
    if (pixels.size() != uint64_t{s.width} * s.height) return false;
    for (uint64_t i = 0; i < pixels.size(); ++i) s.pixels[i] = s.encode(pixels[i]);
    return true;
  }
  bool read_subresource(uint32_t level, uint32_t layer, ArrayView<uint32_t> out) override {
    if (color_.empty() || desc_.samples != 1 || !has(level, layer)) return false;
    const Surface s = surface(level, layer);
    const uint64_t count = uint64_t{s.width} * s.height;
    if (out.size() < count) return false;
    for (uint64_t i = 0; i < count; ++i) out[i] = s.decode(s.pixels[i]);
    return true;
  }
  bool write_subresource_float(uint32_t level, uint32_t layer, ArrayView<const Vec4> pixels) override {
    if (hdr_.empty() || !has(level, layer)) return false;
    const Surface s = surface(level, layer);
    if (pixels.size() != uint64_t{s.width} * s.height) return false;
    for (uint64_t i = 0; i < pixels.size(); ++i) s.floats[i] = store_float(pixels[i]);
    return true;
  }
  bool read_subresource_float(uint32_t level, uint32_t layer, ArrayView<Vec4> out) override {
    if (hdr_.empty() || !has(level, layer)) return false;
    const Surface s = surface(level, layer);
    const uint64_t count = uint64_t{s.width} * s.height;
    if (out.size() < count) return false;
    for (uint64_t i = 0; i < count; ++i) out[i] = s.floats[i];
    return true;
  }
  bool write_layer_pixels(uint32_t layer, ArrayView<const uint32_t> pixels) override { return write_subresource(0, layer, pixels); }
  bool read_layer_pixels(uint32_t layer, ArrayView<uint32_t> out) override { return read_subresource(0, layer, out); }
  bool write_layer_pixels_float(uint32_t layer, ArrayView<const Vec4> pixels) override { return desc_.kind != TextureKind::Texture2D && write_subresource_float(0, layer, pixels); }
  bool read_layer_pixels_float(uint32_t layer, ArrayView<Vec4> out) override { return read_subresource_float(0, layer, out); }
  bool write_mip_pixels(uint32_t level, ArrayView<const uint32_t> pixels) override { return write_subresource(level, 0, pixels); }
  bool read_mip_pixels(uint32_t level, ArrayView<uint32_t> out) override { return read_subresource(level, 0, out); }
  Extent2D extent() const override { return desc_.extent; }
  Format format() const override { return desc_.format; }
  uint32_t usage() const { return desc_.usage; }
  bool read_pixels(ArrayView<uint32_t> out) override {
    const uint64_t count = level_pixels(0);
    if (color_.empty() || out.size() < count || desc_.samples > 1) return false;   // (a multisampled image is resolved first)
    const Surface s = surface();
    if (s.format != Format::R8G8B8A8Unorm && s.format != Format::R8G8B8A8Srgb) return out.first(count).copy_from(ArrayView<const uint32_t>(color_.data(), count));   // (stored as it is read: one copy)
    for (uint64_t i = 0; i < count; ++i) out[i] = s.decode(color_[i]);
    return true;
  }
  bool write_pixels(ArrayView<const uint32_t> pixels) override { return write_subresource(0, 0, pixels); }
  bool read_pixels_float(ArrayView<Vec4> out) override { return layer_count() == 1 && read_subresource_float(0, 0, out); }
  bool write_pixels_float(ArrayView<const Vec4> pixels) override { return layer_count() == 1 && write_subresource_float(0, 0, pixels); }
  // What a pixel of this image keeps of a colour (WidePixels.hpp).
  Vec4 store_float(Vec4 c) const { return quantize_wide_pixel(desc_.format, c); }
  bool write_rect(const Rect2D& area, ArrayView<const uint32_t> pixels, uint32_t pitch) override {
    if (color_.empty() || desc_.samples != 1 || !area.width || !area.height || pitch < area.width) return false;
    const Extent2D e = desc_.extent;
    if (uint64_t{static_cast<uint32_t>(area.x < 0 ? -1 : area.x)} + area.width > e.width || area.x < 0 || area.y < 0 || uint64_t{static_cast<uint32_t>(area.y)} + area.height > e.height) return false;
    if (pixels.size() < uint64_t{area.height - 1} * pitch + area.width) return false;
    const Surface s = surface(0);
    for (uint32_t j = 0; j < area.height; ++j)
      for (uint32_t i = 0; i < area.width; ++i) s.pixels[uint64_t{static_cast<uint32_t>(area.y) + j} * s.pitch + static_cast<uint32_t>(area.x) + i] = s.encode(pixels[uint64_t{j} * pitch + i]);
    return true;
  }
  // Each level from the one before it: a 2 x 2 box of the same layer (an array's, a cube's), or of a volume the 2 x 2 x 2 box of the two slices at twice the slice's depth (the last
  // repeated where the level before has an odd number). 8-bit colours average their codes to the nearest (an sRGB image's chain is of its stored codes), wide ones their values
  // (kept as the format keeps them).
  bool generate_mipmaps() override {
    if ((color_.empty() && hdr_.empty()) || desc_.mip_levels < 2 || desc_.samples != 1) return false;
    const bool volume = desc_.kind == TextureKind::Texture3D;
    const uint32_t taps = volume ? 8 : 4;
    for (uint32_t level = 1; level < desc_.mip_levels; ++level) {
      const uint32_t below = level_layers(level - 1);
      for (uint32_t layer = 0; layer < level_layers(level); ++layer) {
        const uint32_t z0 = volume ? (2 * layer < below ? 2 * layer : below - 1) : layer, z1 = volume ? (2 * layer + 1 < below ? 2 * layer + 1 : below - 1) : layer;
        const Surface a = surface(level - 1, z0), b = surface(level - 1, z1), dst = surface(level, layer);
        for (uint32_t y = 0; y < dst.height; ++y)
          for (uint32_t x = 0; x < dst.width; ++x) {
            const uint32_t y0 = 2 * y < a.height ? 2 * y : a.height - 1, y1 = 2 * y + 1 < a.height ? 2 * y + 1 : a.height - 1;
            const uint32_t x0 = 2 * x < a.width ? 2 * x : a.width - 1, x1 = 2 * x + 1 < a.width ? 2 * x + 1 : a.width - 1;
            const uint64_t at[4] = {uint64_t{y0} * a.width + x0, uint64_t{y0} * a.width + x1, uint64_t{y1} * a.width + x0, uint64_t{y1} * a.width + x1};
            if (!hdr_.empty()) {
              Vec4 sum = {0, 0, 0, 0};
              for (uint64_t i : at) {
                sum = sum + a.floats[i];
                if (volume) sum = sum + b.floats[i];
              }
              dst.floats[uint64_t{y} * dst.width + x] = store_float(sum * (1.0f / static_cast<float>(taps)));
              continue;
            }
            uint32_t sum[4] = {};
            for (uint64_t i : at)
              for (uint32_t c = 0; c < 4; ++c) {
                sum[c] += (a.decode(a.pixels[i]) >> (8 * c)) & 0xFF;
                if (volume) sum[c] += (b.decode(b.pixels[i]) >> (8 * c)) & 0xFF;
              }
            uint32_t averaged = 0;
            for (uint32_t c = 0; c < 4; ++c) averaged |= ((sum[c] + taps / 2) / taps) << (8 * c);
            dst.pixels[uint64_t{y} * dst.width + x] = dst.encode(averaged);
          }
      }
    }
    return true;
  }
  // Layer `layer` of level `level` (an empty surface for one the image does not have).
  Surface surface(uint32_t level = 0, uint32_t layer = 0) const {
    Surface s;
    s.format = desc_.format;
    if (!has(level, layer)) return s;
    const uint64_t count = level_pixels(level), offset = level_offset_[level] + uint64_t{layer} * count;
    s.width = mip_extent(desc_.extent.width, level);
    s.height = mip_extent(desc_.extent.height, level);
    if (!hdr_.empty()) s.floats = hdr_.subview(offset, count);
    else if (!color_.empty()) s.pixels = color_.subview(offset, count);
    s.pitch = s.width * desc_.samples;   // (a multisampled image: the samples of a pixel are neighbours, so the pixel x is at x * samples)
    return s;
  }
  ArrayView<float> depth() const { return depth_; }
  ArrayView<uint8_t> stencil() const { return stencil_; }
  // The depth and stencil planes of one layer of a level of a depth image (all its samples; empty for a level or layer it does not have).
  ArrayView<float> depth_layer(uint32_t layer, uint32_t level = 0) const {
    return has(level, layer) && !depth_.empty() ? depth_.subview(level_offset_[level] + uint64_t{layer} * level_pixels(level), level_pixels(level)) : ArrayView<float>();
  }
  ArrayView<uint8_t> stencil_layer(uint32_t layer, uint32_t level = 0) const {
    return has(level, layer) && !stencil_.empty() ? stencil_.subview(level_offset_[level] + uint64_t{layer} * level_pixels(level), level_pixels(level)) : ArrayView<uint8_t>();
  }

 private:
  uint64_t level_pixels(uint32_t level) const { return uint64_t{mip_extent(desc_.extent.width, level)} * mip_extent(desc_.extent.height, level) * desc_.samples; }
  allocators::IAllocator& allocator_;
  ImageDesc desc_;
  ArrayView<uint32_t> color_;
  ArrayView<Vec4> hdr_;   // the pixels of a float format
  ArrayView<float> depth_;
  ArrayView<uint8_t> stencil_;
  uint64_t level_offset_[kMaxMipLevels] = {};   // (in values, from the start of the storage)
};
class SoftwareShaderModule final : public IShaderModule {
 public:
  explicit SoftwareShaderModule(const ShaderModuleDesc& desc) : desc_(desc) {}
  ShaderStage stage() const override { return desc_.stage; }
  const ShaderModuleDesc& desc() const { return desc_; }

 private:
  ShaderModuleDesc desc_;
};

class SoftwareComputePipeline final : public IPipeline {
 public:
  SoftwareComputePipeline(ComputeShaderFn function, ComputeGroupShaderFn group_function, const uint32_t group[3]) : function_(function), group_function_(group_function) {
    for (uint32_t i = 0; i < 3; ++i) group_[i] = group[i];
  }
  ComputeShaderFn function() const { return function_; }
  ComputeGroupShaderFn group_function() const { return group_function_; }
  uint32_t group(uint32_t axis) const { return group_[axis]; }

 private:
  ComputeShaderFn function_;
  ComputeGroupShaderFn group_function_;
  uint32_t group_[3];
};

class SoftwarePipeline final : public IPipeline {
 public:
  SoftwarePipeline(const GraphicsPipelineDesc& desc, VertexShaderFn vs, FragmentShaderFn fs, TexturedFragmentShaderFn textured, VertexShaderFnEx vs_ex, FragmentShaderFnEx fs_ex)
      : desc_(desc), vertex_(vs), fragment_(fs), textured_(textured), vertex_ex_(vs_ex), fragment_ex_(fs_ex) {}
  const GraphicsPipelineDesc& desc() const { return desc_; }
  VertexShaderFn vertex() const { return vertex_; }
  FragmentShaderFn fragment() const { return fragment_; }
  TexturedFragmentShaderFn textured() const { return textured_; }
  VertexShaderFnEx vertex_ex() const { return vertex_ex_; }
  FragmentShaderFnEx fragment_ex() const { return fragment_ex_; }
  // Does the fragment shader read textures (so that a pixel needs the derivatives of its quad)?
  bool samples_textures() const { return textured_ != nullptr || fragment_ex_ != nullptr; }

 private:
  GraphicsPipelineDesc desc_;
  VertexShaderFn vertex_;
  FragmentShaderFn fragment_;
  TexturedFragmentShaderFn textured_;
  VertexShaderFnEx vertex_ex_;
  FragmentShaderFnEx fragment_ex_;
};

// The textures bound for the draws being made, and the sampling itself (see SamplerDesc for what the coordinates mean). A unit sees its image through a view (bind_texture_view): the
// view's levels are the unit's levels 0 .., its layers the unit's layers, and its kind the kind the lookups take.
class TextureSet final : public ITextureSet {
 public:
  struct Bound {
    const SoftwareImage* image = nullptr;
    TextureKind kind = TextureKind::Texture2D;
    uint32_t base_level = 0, levels = 1, first_layer = 0, layers = 1;
  };
  void bind(uint32_t unit, const SoftwareImage* image, const SamplerDesc& sampler) {
    Bound b;
    b.image = image;
    if (image) b.kind = image->kind(), b.levels = image->mip_levels(), b.layers = image->layer_count();
    bound_[unit] = b;
    samplers_[unit] = sampler;
  }
  // The same through a view; false (and nothing bound) for a view the image cannot have.
  bool bind_view(uint32_t unit, const SoftwareImage* image, const SamplerDesc& sampler, const TextureView& requested) {
    TextureView view = requested;
    if (view.level_count == 0 && view.base_level < image->mip_levels()) view.level_count = image->mip_levels() - view.base_level;
    if (view.layer_count == 0 && view.first_layer < image->layer_count()) view.layer_count = image->layer_count() - view.first_layer;
    if (!texture_view_fits(image->kind(), image->layer_count(), image->mip_levels(), view)) {
      bound_[unit] = Bound();
      return false;
    }
    Bound b;
    b.image = image;
    b.kind = view.kind;
    b.base_level = view.base_level, b.levels = view.level_count;
    b.first_layer = view.first_layer, b.layers = view.layer_count;
    bound_[unit] = b;
    samplers_[unit] = sampler;
    return true;
  }
  const SoftwareImage* image(uint32_t unit) const { return unit < kMaxTextureUnits ? bound_[unit].image : nullptr; }
  // Whether the view of `unit` shows any of the layers first .. first + count - 1 of level `level` of `image` (a pass that draws there may not sample it).
  bool overlaps(uint32_t unit, const SoftwareImage* image, uint32_t level, uint32_t first, uint32_t count) const {
    const Bound& b = bound_[unit];
    if (!b.image || b.image != image || level < b.base_level || level >= b.base_level + b.levels) return false;
    if (b.kind == TextureKind::Texture3D) return true;
    return first < b.first_layer + b.layers && b.first_layer < first + count;
  }

  // Whether any bound unit reads mip levels or filters anisotropically: only then does a pixel need its quad's derivatives.
  bool uses_lod() const {
    for (uint32_t unit = 0; unit < kMaxTextureUnits; ++unit)
      if (bound_[unit].image && ((bound_[unit].levels > 1 && samplers_[unit].mip_filter != MipFilter::None) || anisotropic(samplers_[unit]) || filters_differ(samplers_[unit]))) return true;
    return false;
  }
  // For the derivative passes: the next lookups record their first coordinates per unit and answer black.
  void start_recording() const {
    recording_ = true;
    for (uint32_t unit = 0; unit < kMaxTextureUnits; ++unit) recorded_[unit] = false;
  }
  void stop_recording() const { recording_ = false; }
  bool recorded(uint32_t unit, float* coord) const {
    if (unit >= kMaxTextureUnits || !recorded_[unit]) return false;
    for (int k = 0; k < 3; ++k) coord[k] = record_[unit][k];
    return true;
  }
  // The level of detail the pixel's lookups of `unit` use, from the coordinates recorded at the quad's anchor and at its right and lower neighbours: (u, v) of a 2D image or an array, (u, v, w)
  // of a volume, the direction of a cube (the points on the face the anchor's direction picks).
  void set_lod(uint32_t unit, const float anchor[3], const float right[3], const float below[3]) const {
    if (unit >= kMaxTextureUnits || !bound_[unit].image) return;
    const Bound& b = bound_[unit];
    const SamplerDesc& sampler = samplers_[unit];
    const Extent2D e = b.image->level_extent(b.base_level);
    const float w = static_cast<float>(e.width), h = static_cast<float>(e.height);
    float dx[3] = {}, dy[3] = {};
    if (b.kind == TextureKind::TextureCube || b.kind == TextureKind::TextureCubeArray) {
      const uint32_t face = cube_face_of(anchor[0], anchor[1], anchor[2]);
      float s0, t0, s1, t1, s2, t2;
      cube_on_face(face, anchor[0], anchor[1], anchor[2], &s0, &t0);
      cube_on_face(face, right[0], right[1], right[2], &s1, &t1);
      cube_on_face(face, below[0], below[1], below[2], &s2, &t2);
      dx[0] = (s1 - s0) * w, dx[1] = (t1 - t0) * h, dy[0] = (s2 - s0) * w, dy[1] = (t2 - t0) * h;
    } else {
      dx[0] = (right[0] - anchor[0]) * w, dx[1] = (right[1] - anchor[1]) * h, dy[0] = (below[0] - anchor[0]) * w, dy[1] = (below[1] - anchor[1]) * h;
      if (b.kind == TextureKind::Texture3D) {
        const float d = static_cast<float>(b.image->level_layers(b.base_level));
        dx[2] = (right[2] - anchor[2]) * d, dy[2] = (below[2] - anchor[2]) * d;
      }
    }
    const float rho_x = dx[0] * dx[0] + dx[1] * dx[1] + dx[2] * dx[2], rho_y = dy[0] * dy[0] + dy[1] * dy[1] + dy[2] * dy[2];
    float rho2 = rho_x > rho_y ? rho_x : rho_y;   // (the square of rho: log2(rho) is half of log2(rho2))
    aniso_count_[unit] = 1;
    if (anisotropic(sampler) && rho2 > 0.0f && (b.kind == TextureKind::Texture2D || b.kind == TextureKind::Texture2DArray)) {   // (along (u, v): of a 2D image or an array)
      // (the longer axis of the footprint, Pmax, and the shorter, Pmin: N lookups along the longer one at the level of Pmax / N)
      const float p_max = sqrt_of(rho2), p_min = sqrt_of(rho_x > rho_y ? rho_y : rho_x);
      float ratio = p_min > 0.0f ? p_max / p_min : static_cast<float>(sampler.max_anisotropy);
      if (!(ratio < static_cast<float>(sampler.max_anisotropy))) ratio = static_cast<float>(sampler.max_anisotropy);
      uint32_t n = static_cast<uint32_t>(ratio);
      if (static_cast<float>(n) < ratio) ++n;   // (ceil)
      if (n < 1) n = 1;
      aniso_count_[unit] = n;
      const bool along_x = rho_x >= rho_y;
      aniso_du_[unit] = along_x ? right[0] - anchor[0] : below[0] - anchor[0];
      aniso_dv_[unit] = along_x ? right[1] - anchor[1] : below[1] - anchor[1];
      rho2 /= static_cast<float>(n) * static_cast<float>(n);
    }
    float lambda = 0.5f * mip_log2(rho2) + sampler.lod_bias;
    if (lambda < sampler.min_lod) lambda = sampler.min_lod;
    if (lambda > sampler.max_lod) lambda = sampler.max_lod;
    lambda_[unit] = lambda == lambda ? lambda : 0.0f;
  }
  void clear_lod() const {
    for (uint32_t unit = 0; unit < kMaxTextureUnits; ++unit) lambda_[unit] = 0.0f, aniso_count_[unit] = 1;
  }

  Vec4 sample_volume(uint32_t unit, float u, float v, float w) const override {
    const Bound* b = bound_for(unit);
    if (!b) return {0, 0, 0, 1};
    if (b->kind == TextureKind::Texture2D) return sample(unit, u, v);
    if (recording_) return record(unit, u, v, w);
    return anisotropic_lookup(unit, *b, u, v, w);
  }
  Vec4 sample_cube_array(uint32_t unit, float x, float y, float z, float cube) const override {
    const Bound* b = bound_for(unit);
    if (!b) return {0, 0, 0, 1};
    if (b->kind != TextureKind::TextureCubeArray) return sample_volume(unit, x, y, z);
    if (recording_) return record(unit, x, y, z);
    return lookup(*b, samplers_[unit], x, y, z, cube, lambda_[unit]);
  }

  Vec4 sample(uint32_t unit, float u, float v) const override {
    const Bound* b = bound_for(unit);
    if (!b) return {0, 0, 0, 1};
    if (recording_) return record(unit, u, v, 0.0f);
    return anisotropic_lookup(unit, *b, u, v, 0.0f);
  }

  // An explicit level of detail: clamped as the derivatives' would be.
  Vec4 sample_lod(uint32_t unit, float u, float v, float lod) const override {
    const Bound* b = bound_for(unit);
    if (!b) return {0, 0, 0, 1};
    if (recording_) return sample(unit, u, v);
    return lookup(*b, samplers_[unit], u, v, 0.0f, 0.0f, clamp_lod(samplers_[unit], lod));
  }
  Vec4 sample_lod_volume(uint32_t unit, float u, float v, float w, float lod) const override {
    const Bound* b = bound_for(unit);
    if (!b) return {0, 0, 0, 1};
    if (b->kind == TextureKind::Texture2D) return sample_lod(unit, u, v, lod);
    if (recording_) return record(unit, u, v, w);
    return lookup(*b, samplers_[unit], u, v, w, 0.0f, clamp_lod(samplers_[unit], lod));
  }
  Vec4 sample_lod_cube_array(uint32_t unit, float x, float y, float z, float cube, float lod) const override {
    const Bound* b = bound_for(unit);
    if (!b) return {0, 0, 0, 1};
    if (b->kind != TextureKind::TextureCubeArray) return sample_lod_volume(unit, x, y, z, lod);
    if (recording_) return record(unit, x, y, z);
    return lookup(*b, samplers_[unit], x, y, z, cube, clamp_lod(samplers_[unit], lod));
  }

  Vec4 fetch(uint32_t unit, int32_t x, int32_t y, uint32_t level) const override {
    const Bound* b = bound_for(unit);
    if (!b || level >= b->levels) return {0, 0, 0, 0};
    const Plane plane = view_plane(*b, level, 0);
    if (x < 0 || y < 0 || static_cast<uint32_t>(x) >= plane.surface.width || static_cast<uint32_t>(y) >= plane.surface.height) return {0, 0, 0, 0};
    return image_texel(plane, samplers_[unit].stencil, x, y);
  }
  Vec4 fetch_volume(uint32_t unit, int32_t x, int32_t y, int32_t z, uint32_t level) const override {
    const Bound* b = bound_for(unit);
    if (!b) return {0, 0, 0, 0};
    if (b->kind == TextureKind::Texture2D) return z == 0 ? fetch(unit, x, y, level) : Vec4{0, 0, 0, 0};
    if (b->kind == TextureKind::TextureCube || b->kind == TextureKind::TextureCubeArray || level >= b->levels || z < 0 || static_cast<uint32_t>(z) >= view_depth(*b, level)) return {0, 0, 0, 0};
    const Plane plane = view_plane(*b, level, static_cast<uint32_t>(z));
    if (x < 0 || y < 0 || static_cast<uint32_t>(x) >= plane.surface.width || static_cast<uint32_t>(y) >= plane.surface.height) return {0, 0, 0, 0};
    return image_texel(plane, samplers_[unit].stencil, x, y);
  }
  Vec4 fetch_sample(uint32_t unit, int32_t x, int32_t y, uint32_t sample) const override {
    const SoftwareImage* image = this->image(unit);
    if (!image || image->samples() < 2 || sample >= image->samples()) return {0, 0, 0, 0};
    const Surface surface = image->surface();
    if (x < 0 || y < 0 || static_cast<uint32_t>(x) >= surface.width || static_cast<uint32_t>(y) >= surface.height) return {0, 0, 0, 0};
    // (the samples of a pixel are neighbours: the pixel x is at x * samples)
    return texel(surface, static_cast<int64_t>(x) * image->samples() + sample, y);
  }
  bool size_volume(uint32_t unit, uint32_t level, uint32_t* width, uint32_t* height, uint32_t* depth, uint32_t* levels) const override {
    if (!size(unit, level, width, height, levels)) return false;
    const Bound& b = bound_[unit];
    *depth = b.kind == TextureKind::Texture3D ? (level < b.levels ? view_depth(b, level) : 0) : b.kind == TextureKind::Texture2DArray ? b.layers : b.kind == TextureKind::TextureCubeArray ? b.layers / 6 : 1;
    return true;
  }

  CompareOp compare_op(uint32_t unit) const override { return unit < kMaxTextureUnits ? samplers_[unit].compare : CompareOp::Never; }

  // SampleCmp / SampleCmpLevelZero: the comparison of each texel the filter would read with the reference, filtered as the texels would be (see SamplerDesc::compare). A 2D image or an
  // array's layer (w rounded, as sample_volume takes it) or a cube's face (the direction's), at the level the derivatives give - or level 0 -, the levels blended as sample() blends them.
  float sample_compare(uint32_t unit, float u, float v, float w, float reference, bool level_zero) const override {
    const Bound* b = bound_for(unit);
    if (!b || b->kind == TextureKind::Texture3D || b->kind == TextureKind::TextureCubeArray) return 0.0f;
    if (recording_) {
      if (b->kind != TextureKind::TextureCube) (void)sample(unit, u, v);   // (the coordinates the derivatives are taken from)
      return 0.0f;
    }
    SamplerDesc sampler = samplers_[unit];
    uint32_t layer = 0;
    if (b->kind == TextureKind::TextureCube) {
      uint32_t face = 0;
      if (!cube_face(u, v, w, &face, &u, &v)) return compare_level(view_plane(*b, 0, 0), sampler, 0.5f, 0.5f, reference);
      layer = face;
      sampler.address_u = sampler.address_v = AddressMode::ClampToEdge;   // (no filtering across faces)
    } else if (b->kind == TextureKind::Texture2DArray) {
      int64_t l = floor_to(unit_coordinate(w) + 0.5f);
      layer = static_cast<uint32_t>(l < 0 ? 0 : l >= b->layers ? b->layers - 1 : l);
    }
    const float lambda = level_zero ? 0.0f : lambda_[unit];
    sampler = at_lod(sampler, lambda);
    return filtered(*b, sampler, lambda, [&](uint32_t level) {
      const float value = compare_level(view_plane(*b, level, layer), sampler, u, v, reference);
      return Vec4{value, value, value, value};
    }).x;
  }

  bool size(uint32_t unit, uint32_t level, uint32_t* width, uint32_t* height, uint32_t* levels) const override {
    const Bound* b = bound_for(unit);
    if (!b) return false;
    const uint32_t at = level < kMaxMipLevels ? b->base_level + level : kMaxMipLevels;
    *width = mip_extent(b->image->extent().width, at);
    *height = mip_extent(b->image->extent().height, at);
    *levels = b->levels;
    return true;
  }

 private:
  const Bound* bound_for(uint32_t unit) const { return unit < kMaxTextureUnits && bound_[unit].image ? &bound_[unit] : nullptr; }
  // A lookup with the unit's level of detail - of an anisotropic filter (aniso_count_ above 1, set_lod): that many lookups spread evenly over the longer axis of the footprint, averaged.
  Vec4 anisotropic_lookup(uint32_t unit, const Bound& b, float u, float v, float w) const {
    const uint32_t n = aniso_count_[unit];
    if (n <= 1) return lookup(b, samplers_[unit], u, v, w, 0.0f, lambda_[unit]);
    const SamplerReduction reduction = samplers_[unit].reduction;
    Vec4 sum{0, 0, 0, 0};
    for (uint32_t k = 0; k < n; ++k) {
      const float t = (static_cast<float>(k) + 0.5f) / static_cast<float>(n) - 0.5f;
      const Vec4 one = lookup(b, samplers_[unit], u + aniso_du_[unit] * t, v + aniso_dv_[unit] * t, w, 0.0f, lambda_[unit]);
      sum = reduction == SamplerReduction::Weighted ? sum + one : k == 0 ? one : reduce(sum, one, reduction);   // (a minimum or maximum: of every lookup)
    }
    return reduction == SamplerReduction::Weighted ? sum * (1.0f / static_cast<float>(n)) : sum;
  }
  Vec4 record(uint32_t unit, float a, float b, float c) const {
    if (!recorded_[unit]) {
      recorded_[unit] = true;
      record_[unit][0] = a, record_[unit][1] = b, record_[unit][2] = c;
    }
    return {0, 0, 0, 1};
  }
  static float clamp_lod(const SamplerDesc& sampler, float lod) {
    float lambda = lod == lod ? lod : 0.0f;
    if (lambda < sampler.min_lod) lambda = sampler.min_lod;
    if (lambda > sampler.max_lod) lambda = sampler.max_lod;
    return lambda;
  }
  // One layer of one level a lookup reads: its surface, and of a depth image (kFeature2DepthTextures) its depth and stencil planes there - the surface then only says the size.
  struct Plane {
    Surface surface;
    ArrayView<float> depth;
    ArrayView<uint8_t> stencil;
  };
  // Layer `layer` of the view's level `level` (of a volume's: its slice there), with the planes of a depth image.
  static Plane view_plane(const Bound& b, uint32_t level, uint32_t layer) {
    const uint32_t at_level = b.base_level + level, at_layer = b.kind == TextureKind::Texture3D ? layer : b.first_layer + layer;
    Plane p;
    p.surface = b.image->surface(at_level, at_layer);
    if (b.image->format() == Format::D32Float) p.depth = b.image->depth_layer(at_layer, at_level), p.stencil = b.image->stencil_layer(at_layer, at_level);
    return p;
  }
  // The layers of the view at its level `level`: a volume's slices there, the view's layers for the others.
  static uint32_t view_depth(const Bound& b, uint32_t level) { return b.kind == TextureKind::Texture3D ? b.image->level_layers(b.base_level + level) : b.layers; }

  // The face of a cube a direction picks: the axis it is longest along (+X, -X, +Y, -Y, +Z, -Z).
  static uint32_t cube_face_of(float x, float y, float z) {
    const float ax = x < 0 ? -x : x, ay = y < 0 ? -y : y, az = z < 0 ? -z : z;
    if (ax >= ay && ax >= az) return x >= 0 ? 0 : 1;
    if (ay >= az) return y >= 0 ? 2 : 3;
    return z >= 0 ? 4 : 5;
  }
  // Where a direction meets the plane of face `face`, 0 .. 1 across the face (the usual table: +X looks along -z with t down -y, ...); false when it does not point at that plane.
  static bool cube_on_face(uint32_t face, float x, float y, float z, float* s, float* t) {
    float major, sc, tc;
    switch (face) {
      case 0: major = x, sc = -z, tc = -y; break;
      case 1: major = -x, sc = z, tc = -y; break;
      case 2: major = y, sc = x, tc = z; break;
      case 3: major = -y, sc = x, tc = -z; break;
      case 4: major = z, sc = x, tc = -y; break;
      default: major = -z, sc = -x, tc = -y; break;
    }
    if (!(major > 0.0f)) {
      *s = *t = 0.5f;
      return false;
    }
    *s = (unit_coordinate(sc / major) + 1.0f) * 0.5f;
    *t = (unit_coordinate(tc / major) + 1.0f) * 0.5f;
    return true;
  }
  // The levels of the view filtered as SamplerDesc says: `at(level)` is the lookup in one of them.
  template <typename At>
  static Vec4 filtered(const Bound& b, const SamplerDesc& sampler, float lambda, At at) {
    const uint32_t levels = b.levels;
    if (sampler.mip_filter == MipFilter::None || levels < 2) return at(0u);
    const float top = static_cast<float>(levels - 1);
    if (lambda > top) lambda = top;
    if (!(lambda > 0.0f)) return at(0u);   // (magnification)
    if (sampler.mip_filter == MipFilter::Nearest) {
      uint32_t level = static_cast<uint32_t>(lambda + 0.5f);
      if (level > levels - 1) level = levels - 1;
      return at(level);
    }
    const uint32_t l0 = static_cast<uint32_t>(lambda), l1 = l0 + 1 < levels ? l0 + 1 : levels - 1;
    const float fraction = lambda - static_cast<float>(l0);
    if (sampler.reduction != SamplerReduction::Weighted && !(fraction > 0.0f)) return at(l0);   // (a level of weight 0 is not read by a minimum or maximum)
    return mix_reduced(at(l0), at(l1), fraction, sampler.reduction);
  }
  // Two values blended by `t` (the weight of b), or - a minimum or maximum reduction - the least or greatest of those whose weight is above 0, channel by channel.
  static Vec4 reduce(const Vec4& a, const Vec4& b, SamplerReduction r) {
    auto pick = [r](float x, float y) { return r == SamplerReduction::Minimum ? (y < x ? y : x) : (y > x ? y : x); };
    return {pick(a.x, b.x), pick(a.y, b.y), pick(a.z, b.z), pick(a.w, b.w)};
  }
  static Vec4 mix_reduced(const Vec4& a, const Vec4& b, float t, SamplerReduction r) {
    if (r == SamplerReduction::Weighted) return a * (1.0f - t) + b * t;
    if (!(t > 0.0f)) return a;
    if (!(t < 1.0f)) return b;
    return reduce(a, b, r);
  }
  // The sampler as a lookup at level of detail `lambda` filters: its minification filter where lambda is above 0, its magnification filter elsewhere.
  static SamplerDesc at_lod(const SamplerDesc& given, float lambda) {
    SamplerDesc s = given;
    if (s.separate_mag && !(lambda > 0.0f)) s.filter = s.mag_filter;
    s.separate_mag = false;
    return s;
  }
  static bool filters_differ(const SamplerDesc& s) { return s.separate_mag && s.mag_filter != s.filter && !s.unnormalized; }
  // A lookup in the view at a level of detail: (u, v) of a 2D image, and w the layer of an array, the depth of a volume, or with (u, v) the direction of a cube (whose number in a
  // cube array is `cube`).
  static Vec4 lookup(const Bound& b, const SamplerDesc& given, float u, float v, float w, float cube, float lambda) {
    if (given.unnormalized && (b.kind == TextureKind::Texture2D || b.kind == TextureKind::Texture2DArray)) {
      // (coordinates in texels of the view's first level, which every lookup reads)
      const Extent2D e = b.image->level_extent(b.base_level);
      u = unit_coordinate(u) / static_cast<float>(e.width ? e.width : 1);
      v = unit_coordinate(v) / static_cast<float>(e.height ? e.height : 1);
      lambda = 0.0f;
    }
    SamplerDesc sampler = at_lod(given, lambda);
    w = unit_coordinate(w);
    switch (b.kind) {
      case TextureKind::TextureCube: case TextureKind::TextureCubeArray: {
        uint32_t layer = 0;
        if (b.kind == TextureKind::TextureCubeArray) {
          const uint32_t cubes = b.layers / 6;
          int64_t n = floor_to(unit_coordinate(cube) + 0.5f);
          n = n < 0 ? 0 : n >= cubes ? cubes - 1 : n;
          layer = static_cast<uint32_t>(n) * 6;
        }
        const uint32_t face = cube_face_of(unit_coordinate(u), unit_coordinate(v), w);
        float s, t;
        if (!cube_on_face(face, unit_coordinate(u), unit_coordinate(v), w, &s, &t)) return sample_level(view_plane(b, 0, layer), sampler, 0.5f, 0.5f);
        sampler.address_u = sampler.address_v = AddressMode::ClampToEdge;
        return filtered(b, sampler, lambda, [&](uint32_t level) { return sample_level(view_plane(b, level, layer + face), sampler, s, t); });
      }
      case TextureKind::Texture2DArray: {
        int64_t layer = floor_to(w + 0.5f);
        layer = layer < 0 ? 0 : layer >= b.layers ? b.layers - 1 : layer;
        return filtered(b, sampler, lambda, [&](uint32_t level) { return sample_level(view_plane(b, level, static_cast<uint32_t>(layer)), sampler, u, v); });
      }
      case TextureKind::Texture3D:
        // A volume: the slice the point lies in, or the two around it blended, of each level.
        return filtered(b, sampler, lambda, [&](uint32_t level) {
          const uint32_t layers = view_depth(b, level);
          const float depth = static_cast<float>(layers);
          auto clamp_layer = [&](int64_t i) { return static_cast<uint32_t>(i < 0 ? 0 : i >= layers ? layers - 1 : i); };
          if (sampler.filter == Filter::Nearest) return sample_level(view_plane(b, level, clamp_layer(floor_to(w * depth))), sampler, u, v);
          const float fz = w * depth - 0.5f;
          const int64_t z0 = floor_to(fz);
          const float tz = fz - static_cast<float>(z0);
          const Vec4 a = sample_level(view_plane(b, level, clamp_layer(z0)), sampler, u, v), c = sample_level(view_plane(b, level, clamp_layer(z0 + 1)), sampler, u, v);
          return mix_reduced(a, c, tz, sampler.reduction);
        });
      default:
        return filtered(b, sampler, lambda, [&](uint32_t level) { return sample_level(view_plane(b, level, 0), sampler, u, v); });
    }
  }

  // The face of a cube a direction points at and the place on it (0..1 both ways): the usual table, as sample_volume has it. False for the zero direction.
  static bool cube_face(float x, float y, float z, uint32_t* face, float* u, float* v) {
    x = unit_coordinate(x), y = unit_coordinate(y), z = unit_coordinate(z);
    const float ax = x < 0 ? -x : x, ay = y < 0 ? -y : y, az = z < 0 ? -z : z;
    float major, sc, tc;
    if (ax >= ay && ax >= az) *face = x >= 0 ? 0 : 1, major = ax, sc = x >= 0 ? -z : z, tc = -y;
    else if (ay >= az) *face = y >= 0 ? 2 : 3, major = ay, sc = x, tc = y >= 0 ? z : -z;
    else *face = z >= 0 ? 4 : 5, major = az, sc = z >= 0 ? x : -x, tc = -y;
    if (!(major > 0.0f)) return false;
    *u = (sc / major + 1.0f) * 0.5f;
    *v = (tc / major + 1.0f) * 0.5f;
    return true;
  }
  // One level's comparison: reference <op> each texel the sampler's filter reads (a texel outside an image with a border: the border colour's red), filtered by that filter.
  static float compare_level(const Plane& texels, const SamplerDesc& sampler, float u, float v, float reference) {
    const int64_t w = texels.surface.width, h = texels.surface.height;
    if (!w || !h) return 0.0f;
    SamplerDesc plane = sampler;
    plane.stencil = false;   // (what is compared is the depth, or a colour image's red)
    auto pass = [&](int64_t x, int64_t y) { return compare_holds(sampler.compare, reference, texel_or_border(texels, plane, x, y).x) ? 1.0f : 0.0f; };
    if (sampler.filter == Filter::Nearest)
      return pass(index(floor_to(unit_coordinate(u) * static_cast<float>(w)), w, sampler.address_u), index(floor_to(unit_coordinate(v) * static_cast<float>(h)), h, sampler.address_v));
    const float fx = unit_coordinate(u) * static_cast<float>(w) - 0.5f, fy = unit_coordinate(v) * static_cast<float>(h) - 0.5f;
    const int64_t x0 = floor_to(fx), y0 = floor_to(fy);
    const float tx = fx - static_cast<float>(x0), ty = fy - static_cast<float>(y0);
    const int64_t xs[2] = {index(x0, w, sampler.address_u), index(x0 + 1, w, sampler.address_u)};
    const int64_t ys[2] = {index(y0, h, sampler.address_v), index(y0 + 1, h, sampler.address_v)};
    const float top = pass(xs[0], ys[0]) * (1 - tx) + pass(xs[1], ys[0]) * tx, bottom = pass(xs[0], ys[1]) * (1 - tx) + pass(xs[1], ys[1]) * tx;
    return top * (1 - ty) + bottom * ty;
  }

  static Vec4 sample_level(const Plane& texels, const SamplerDesc& sampler, float u, float v) {
    const Surface& surface = texels.surface;
    if (!surface.width || !surface.height) return {0, 0, 0, 0};
    const int64_t w = surface.width, h = surface.height;
    if (sampler.filter == Filter::Nearest || sampler.stencil) {   // (the stencil plane holds integers: never filtered)
      return texel_or_border(texels, sampler, index(floor_to(unit_coordinate(u) * static_cast<float>(w)), w, sampler.address_u), index(floor_to(unit_coordinate(v) * static_cast<float>(h)), h, sampler.address_v));
    }
    // Linear: the four texels around the point, whose centres are half a texel in.
    const float fx = unit_coordinate(u) * static_cast<float>(w) - 0.5f, fy = unit_coordinate(v) * static_cast<float>(h) - 0.5f;
    const int64_t x0 = floor_to(fx), y0 = floor_to(fy);
    const float tx = fx - static_cast<float>(x0), ty = fy - static_cast<float>(y0);
    const int64_t xs[2] = {index(x0, w, sampler.address_u), index(x0 + 1, w, sampler.address_u)};
    const int64_t ys[2] = {index(y0, h, sampler.address_v), index(y0 + 1, h, sampler.address_v)};
    const Vec4 a = texel_or_border(texels, sampler, xs[0], ys[0]), b = texel_or_border(texels, sampler, xs[1], ys[0]), c = texel_or_border(texels, sampler, xs[0], ys[1]),
               d = texel_or_border(texels, sampler, xs[1], ys[1]);
    // (a minimum or maximum: of the texels whose bilinear weight is above 0)
    const Vec4 top = mix_reduced(a, b, tx, sampler.reduction), bottom = mix_reduced(c, d, tx, sampler.reduction);
    return mix_reduced(top, bottom, ty, sampler.reduction);
  }
  // (index() gives -1 for a texel outside an image whose sampler has a border there)
  static bool anisotropic(const SamplerDesc& s) { return s.max_anisotropy > 1 && s.filter == Filter::Linear; }
  static float sqrt_of(float v) { return v > 0.0f ? __builtin_sqrtf(v) : 0.0f; }
  static Vec4 texel_or_border(const Plane& texels, const SamplerDesc& sampler, int64_t x, int64_t y) {
    if (x < 0 || y < 0) return sampler_border_value(sampler);
    return image_texel(texels, sampler.stencil, x, y);
  }
  // A texel of a colour image, or of the depth or stencil plane of a depth image (kFeature2DepthTextures): the depth as (depth, 0, 0, 1), the stencil as the integers (0, stencil, 0, 1).
  static Vec4 image_texel(const Plane& texels, bool stencil, int64_t x, int64_t y) {
    if (texels.surface.format != Format::D32Float) return texel(texels.surface, x, y);
    const uint64_t at = static_cast<uint64_t>(y) * texels.surface.width + static_cast<uint64_t>(x);
    if (stencil) {
      const uint32_t value = at < texels.stencil.size() ? texels.stencil[at] : 0u;
      return {__builtin_bit_cast(float, 0u), __builtin_bit_cast(float, value), __builtin_bit_cast(float, 0u), __builtin_bit_cast(float, 1u)};
    }
    return {at < texels.depth.size() ? texels.depth[at] : 0.0f, 0.0f, 0.0f, 1.0f};
  }
  // A coordinate a NaN or a very large value cannot turn into an index outside the image: both are brought into a range the integer conversion can take.
  static float unit_coordinate(float c) {
    if (!(c == c)) return 0.0f;
    if (c > 1e6f) return 1e6f;
    if (c < -1e6f) return -1e6f;
    return c;
  }
  static int64_t floor_to(float f) {
    const int64_t i = static_cast<int64_t>(f);
    return static_cast<float>(i) > f ? i - 1 : i;
  }
  // The texel column or row an integer coordinate means, for an image `size` texels wide or high.
  static int64_t index(int64_t i, int64_t size, AddressMode mode) {
    if (mode == AddressMode::Repeat) {
      i %= size;
      return i < 0 ? i + size : i;
    }
    if (mode == AddressMode::MirrorRepeat) {   // (every second copy of the image is turned around)
      i %= 2 * size;
      if (i < 0) i += 2 * size;
      return i < size ? i : 2 * size - 1 - i;
    }
    if (mode == AddressMode::ClampToBorder && (i < 0 || i >= size)) return -1;
    if (mode == AddressMode::MirrorOnce && i < 0) i = -1 - i;   // (mirrored about zero, then the edge texel)
    return i < 0 ? 0 : (i >= size ? size - 1 : i);
  }
  static Vec4 texel(const Surface& surface, int64_t x, int64_t y) {
    if (!surface.floats.empty()) return surface.float_row(static_cast<uint32_t>(y))[x];
    const uint32_t argb = surface.decode(surface.row(static_cast<uint32_t>(y))[x]);
    constexpr float k = 1.0f / 255.0f;
    if (is_packed32_format(surface.format)) {   // (the exact values of the 5-, 6- and 4-bit codes)
      float c[4];
      packed32_to_floats(surface.format, argb, c);
      return {c[0], c[1], c[2], c[3]};
    }
    if (is_srgb_format(surface.format)) {   // (the colour channels are decoded to linear values, the alpha is linear)
      const float* table = srgb_to_linear_table();
      return {table[(argb >> 16) & 0xFF], table[(argb >> 8) & 0xFF], table[argb & 0xFF], static_cast<float>(argb >> 24) * k};
    }
    return {static_cast<float>((argb >> 16) & 0xFF) * k, static_cast<float>((argb >> 8) & 0xFF) * k, static_cast<float>(argb & 0xFF) * k, static_cast<float>(argb >> 24) * k};
  }

  Bound bound_[kMaxTextureUnits];
  SamplerDesc samplers_[kMaxTextureUnits];
  // The state of the pixel being shaded (the shader sees this set through a const reference).
  mutable bool recording_ = false;
  mutable bool recorded_[kMaxTextureUnits] = {};
  mutable float record_[kMaxTextureUnits][3] = {};
  mutable float lambda_[kMaxTextureUnits] = {};
  mutable uint32_t aniso_count_[kMaxTextureUnits] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};   // (the lookups of an anisotropic filter, and the axis they are spread over in u, v)
  mutable float aniso_du_[kMaxTextureUnits] = {}, aniso_dv_[kMaxTextureUnits] = {};
};

// The storage images bound (bind_storage_image), as the shaders read and write them: a texel of the view is texel (x, y) of layer first_layer + z (of level `level`). A slot may instead
// hold a typed view of a buffer (bind_storage_texel_buffer): element x of the view, at y = z = 0.
class StorageImageSet final : public IStorageImages {
 public:
  void bind(uint32_t slot, SoftwareImage* image, const StorageImageView& view) {
    if (slot >= kMaxImageSlots) return;
    images_[slot] = image;
    views_[slot] = view;
    elements_[slot] = ByteView();
  }
  // `elements`: the bytes of the view's elements (empty unbinds).
  void bind_texel_buffer(uint32_t slot, ByteView elements, TexelFormat format) {
    if (slot >= kMaxImageSlots) return;
    images_[slot] = nullptr;
    elements_[slot] = elements;
    texel_formats_[slot] = format;
  }
  bool size(uint32_t slot, uint32_t* width, uint32_t* height, uint32_t* layers) const override {
    if (slot < kMaxImageSlots && !elements_[slot].empty()) {
      *width = static_cast<uint32_t>(elements_[slot].size() / texel_bytes(texel_formats_[slot]));
      *height = 1;
      *layers = 1;
      return true;
    }
    if (slot >= kMaxImageSlots || !images_[slot]) return false;
    *width = mip_extent(images_[slot]->extent().width, views_[slot].level);
    *height = mip_extent(images_[slot]->extent().height, views_[slot].level);
    *layers = views_[slot].layer_count;
    return true;
  }
  Vec4 load(uint32_t slot, uint32_t x, uint32_t y, uint32_t z) const override {
    ByteView element;
    if (texel_element(slot, x, y, z, &element)) return element.empty() ? Vec4{0, 0, 0, 0} : decode_texel(texel_formats_[slot], element);   // (outside the view: zero)
    Surface surface;
    if (!texel_surface(slot, x, y, z, &surface)) return {0, 0, 0, 0};
    if (!surface.floats.empty()) return as_view(slot, surface.format, surface.float_row(y)[x]);
    const uint32_t argb = surface.decode(surface.row(y)[x]);
    if (view_format(slot, surface.format) == Format::R8G8B8A8Uint)   // (the bytes as integers)
      return {__builtin_bit_cast(float, (argb >> 16) & 0xFF), __builtin_bit_cast(float, (argb >> 8) & 0xFF), __builtin_bit_cast(float, argb & 0xFF), __builtin_bit_cast(float, argb >> 24)};
    return unpack_color(argb);
  }
  void store(uint32_t slot, uint32_t x, uint32_t y, uint32_t z, Vec4 value) const override {
    ByteView element;
    if (texel_element(slot, x, y, z, &element)) {
      if (slot < kMaxStorageImages) encode_texel(texel_formats_[slot], value, element);   // (the views of t# are read only)
      return;
    }
    Surface surface;
    if (!texel_surface(slot, x, y, z, &surface)) return;
    if (!surface.floats.empty()) {
      const Format view = view_format(slot, surface.format);
      surface.float_row(y)[x] = view == surface.format ? images_[slot]->store_float(value) : as_view(slot, view, quantize_wide_pixel(view, value), surface.format);
      return;
    }
    if (view_format(slot, surface.format) == Format::R8G8B8A8Uint) {   // (integers into the bytes, saturated)
      auto byte = [](float v) { const uint32_t b = __builtin_bit_cast(uint32_t, v); return b > 255 ? 255u : b; };
      surface.row(y)[x] = surface.encode(byte(value.w) << 24 | byte(value.x) << 16 | byte(value.y) << 8 | byte(value.z));
      return;
    }
    surface.row(y)[x] = surface.encode(pack_color(value));
  }
  bool atomic(uint32_t slot, uint32_t x, uint32_t y, uint32_t z, ImageAtomic op, uint32_t operand, uint32_t compare, uint32_t* old) const override {
    *old = 0;
    ByteView element;
    Surface surface;
    uint32_t before = 0;
    Vec4* texel = nullptr;
    if (texel_element(slot, x, y, z, &element)) {
      // (an element of a 32-bit integer view of a buffer: its little-endian word)
      if (element.empty() || slot >= kMaxStorageImages || !texel_format_atomic(texel_formats_[slot])) return false;
      before = ByteReader(element).le32();
    } else {
      // (a 32-bit integer view: of an R32Uint or R32Sint image, or of an R32Float one, whose texel's bits it is)
      if (!texel_surface(slot, x, y, z, &surface) || !is_single_channel_format(surface.format)) return false;
      const Format view = view_format(slot, surface.format);
      if (view != Format::R32Uint && view != Format::R32Sint) return false;
      texel = &surface.float_row(y)[x];
      before = __builtin_bit_cast(uint32_t, texel->x);
    }
    uint32_t after = before;
    switch (op) {
      case ImageAtomic::Add: after = before + operand; break;
      case ImageAtomic::And: after = before & operand; break;
      case ImageAtomic::Or: after = before | operand; break;
      case ImageAtomic::Xor: after = before ^ operand; break;
      case ImageAtomic::IMax: after = static_cast<int32_t>(operand) > static_cast<int32_t>(before) ? operand : before; break;
      case ImageAtomic::IMin: after = static_cast<int32_t>(operand) < static_cast<int32_t>(before) ? operand : before; break;
      case ImageAtomic::UMax: after = operand > before ? operand : before; break;
      case ImageAtomic::UMin: after = operand < before ? operand : before; break;
      case ImageAtomic::Exchange: after = operand; break;
      case ImageAtomic::CompareExchange: after = before == compare ? operand : before; break;
    }
    if (texel) texel->x = __builtin_bit_cast(float, after);
    else ByteWriter(element).le32(after);
    *old = before;
    return true;
  }

 private:
  Format view_format(uint32_t slot, Format image) const { return views_[slot].format == Format::Undefined ? image : views_[slot].format; }
  // A texel of a wide image `from` (the values of that format) as the values of format `to` the same bits are (the image's format to the view's, or back): what a typeless resource's view does.
  Vec4 as_view(uint32_t slot, Format from, Vec4 value, Format to = Format::Undefined) const {
    if (to == Format::Undefined) to = view_format(slot, from);
    if (to == from) return value;
    if (to == Format::R8G8B8A8Unorm || from == Format::R8G8B8A8Unorm) {   // (an R8G8B8A8Uint image's bytes as normalised values, or back)
      const Vec4 q = quantize_wide_pixel(Format::R8G8B8A8Uint, value);
      auto bits = [](float v) { return __builtin_bit_cast(uint32_t, v); };
      if (to == Format::R8G8B8A8Unorm) return {bits(q.x) / 255.0f, bits(q.y) / 255.0f, bits(q.z) / 255.0f, bits(q.w) / 255.0f};
      const uint32_t argb = pack_color(value);
      return {__builtin_bit_cast(float, (argb >> 16) & 0xFF), __builtin_bit_cast(float, (argb >> 8) & 0xFF), __builtin_bit_cast(float, argb & 0xFF), __builtin_bit_cast(float, argb >> 24)};
    }
    uint8_t bits[16] = {};
    ByteWriter out{ByteView(bits, sizeof bits)};
    encode_wide_pixel(from, value, out);
    ByteReader in{ConstByteView(bits, bytes_per_pixel(from))};
    return decode_wide_pixel(to, in);
  }
  // The bytes of element x of a typed view of a buffer (false for a slot that has none, and - as nothing - outside the view: `out` then stays empty).
  bool texel_element(uint32_t slot, uint32_t x, uint32_t y, uint32_t z, ByteView* out) const {
    if (slot >= kMaxImageSlots || elements_[slot].empty()) return false;
    const uint32_t bytes = texel_bytes(texel_formats_[slot]);
    if (y != 0 || z != 0 || !elements_[slot].try_subview(uint64_t{x} * bytes, bytes, out)) *out = ByteView();
    return true;
  }
  // The surface the texel is in (false outside the view, or nothing bound).
  bool texel_surface(uint32_t slot, uint32_t x, uint32_t y, uint32_t z, Surface* out) const {
    if (slot >= kMaxImageSlots || !images_[slot]) return false;
    const StorageImageView& view = views_[slot];
    if (z >= view.layer_count || view.level >= images_[slot]->mip_levels() || view.first_layer + z >= images_[slot]->level_layers(view.level)) return false;
    *out = images_[slot]->surface(view.level, view.first_layer + z);
    return x < out->width && y < out->height;
  }
  SoftwareImage* images_[kMaxImageSlots] = {};
  StorageImageView views_[kMaxImageSlots];
  ByteView elements_[kMaxImageSlots];
  TexelFormat texel_formats_[kMaxImageSlots] = {};
};

// --- Rasterization -------------------------------------------------------------

struct ClipVertex {
  Vec4 position;
  Vec4 varying[kMaxVaryings];
  uint32_t layer = 0;      // (SV_RenderTargetArrayIndex: a primitive goes to its first vertex's)
  uint32_t viewport = 0;   // (SV_ViewportArrayIndex: the same)
};

inline float lerp(float a, float b, float t) { return a + (b - a) * t; }

ClipVertex mix(const ClipVertex& a, const ClipVertex& b, float t, uint32_t varyings) {
  ClipVertex r;
  r.layer = a.layer;
  r.viewport = a.viewport;
  r.position = {lerp(a.position.x, b.position.x, t), lerp(a.position.y, b.position.y, t),
                lerp(a.position.z, b.position.z, t), lerp(a.position.w, b.position.w, t)};
  for (uint32_t i = 0; i < varyings; ++i)
    r.varying[i] = {lerp(a.varying[i].x, b.varying[i].x, t), lerp(a.varying[i].y, b.varying[i].y, t),
                    lerp(a.varying[i].z, b.varying[i].z, t), lerp(a.varying[i].w, b.varying[i].w, t)};
  return r;
}

struct ScreenVertex {
  float x, y, z, inv_w;
  Vec4 varying[kMaxVaryings];
};

// What the draws and dispatches of a submit did, counted from its start: a query is the difference between what they were at its end and at its begin (so any number can be open,
// nested or not, inside passes or outside).
struct QueryCounters {
  uint64_t samples = 0;                                 // fragments (samples) that passed the stencil and depth tests
  uint64_t statistics[kPipelineStatisticCount] = {};   // PipelineStatistic
  uint64_t so[2 * kMaxGeometryStreams] = {};           // for each stream the primitives written (2 * stream) and needed (2 * stream + 1)
};

// Everything one draw call needs.
struct DrawState {
  const SoftwarePipeline* pipeline = nullptr;
  SoftwareImage* color = nullptr;
  SoftwareImage* extra_colors[kMaxColorTargets - 1] = {};
  SoftwareImage* depth = nullptr;
  // The layers of the targets the pass draws into (RenderPassBegin::first_layer / layer_count; kMaxColorTargets: the depth image) and the one the primitive being drawn goes to.
  uint32_t first_layer[kMaxColorTargets + 1] = {};
  uint32_t layer_count[kMaxColorTargets + 1] = {1, 1, 1, 1, 1, 1, 1, 1, 1};
  uint32_t level[kMaxColorTargets + 1] = {};   // (RenderPassBegin::level: the level of each target drawn into)
  Extent2D extent;                             // (the size of the pass: of those levels)
  mutable uint32_t layer = 0;
  void select_layer(uint32_t l) const { layer = l; }
  // (the layer of target n the primitive goes to: its own first when the primitive's is beyond the target's)
  uint32_t layer_of(uint32_t n) const { return first_layer[n] + (layer < layer_count[n] ? layer : 0); }
  Surface target(uint32_t n) const {
    const SoftwareImage* image = n == 0 ? color : extra_colors[n - 1];
    return image->surface(level[n], layer_of(n));
  }
  ArrayView<float> depth_plane() const { return depth ? depth->depth_layer(layer_of(kMaxColorTargets), level[kMaxColorTargets]) : ArrayView<float>(); }
  ArrayView<uint8_t> stencil_plane() const { return depth ? depth->stencil_layer(layer_of(kMaxColorTargets), level[kMaxColorTargets]) : ArrayView<uint8_t>(); }
  const StorageImageSet* images = nullptr;   // the storage images bound
  // The viewports and scissor rectangles set (the scissors clamped to the target), and those of the primitive being drawn: the ones its SV_ViewportArrayIndex names, the first when that is
  // past the ones set.
  Viewport viewports[kMaxViewports];
  Rect2D scissors[kMaxViewports];
  uint32_t viewport_count = 1, scissor_count = 1;
  mutable Viewport viewport;
  mutable Rect2D scissor;
  void select_viewport(uint32_t index) const {
    viewport = viewports[index < viewport_count ? index : 0];
    scissor = scissors[index < viewport_count && index < scissor_count ? index : 0];
  }
  TextureSet textures;
  const uint8_t* constants = nullptr;
  const ByteView* storage = nullptr;   // the storage buffers the shaders of a draw can read
  uint8_t constants_storage[kMaxPushConstantBytes] = {};
  uint32_t primitive_id = 0;   // the primitive of the draw being rasterized (SV_PrimitiveID)
  uint32_t sample_count = 1;   // samples per pixel of the pass's images
  QueryCounters* counters = nullptr;   // what the draws and dispatches of the submit did (for its queries; null outside a submit)
  // The stream output targets (BindStreamOutput): where each buffer's next record goes. `closed` is set once a primitive did not fit.
  struct StreamOutputTarget {
    ByteView view;
    uint64_t written = 0;
    bool closed = false;
    IBuffer* buffer = nullptr;   // (what it is bound to, to remember how far it got)
    uint64_t base = 0;           // (the offset the view starts at)
  };
  StreamOutputTarget* stream_output = nullptr;   // kMaxStreamOutputBuffers of them (null outside a submit)
  void count(PipelineStatistic statistic, uint64_t n = 1) const {
    if (counters) counters->statistics[static_cast<uint32_t>(statistic)] += n;
  }
};

float read_float(ConstByteView bytes, uint64_t offset) {
  float v = 0;
  const uint8_t* src = bytes.try_at(offset);
  if (src && offset + 4 <= bytes.size()) __builtin_memcpy(&v, src, 4);
  return v;
}

bool stencil_compare(CompareOp op, uint32_t reference, uint32_t stored) {
  switch (op) {
    case CompareOp::Never: return false;
    case CompareOp::Less: return reference < stored;
    case CompareOp::Equal: return reference == stored;
    case CompareOp::LessEqual: return reference <= stored;
    case CompareOp::Greater: return reference > stored;
    case CompareOp::NotEqual: return reference != stored;
    case CompareOp::GreaterEqual: return reference >= stored;
    case CompareOp::Always: return true;
  }
  return false;
}

uint8_t stencil_apply(StencilOp op, uint8_t stored, uint8_t reference) {
  switch (op) {
    case StencilOp::Keep: return stored;
    case StencilOp::Zero: return 0;
    case StencilOp::Replace: return reference;
    case StencilOp::IncrementClamp: return stored == 255 ? stored : static_cast<uint8_t>(stored + 1);
    case StencilOp::DecrementClamp: return stored == 0 ? stored : static_cast<uint8_t>(stored - 1);
    case StencilOp::Invert: return static_cast<uint8_t>(~stored);
    case StencilOp::IncrementWrap: return static_cast<uint8_t>(stored + 1);
    case StencilOp::DecrementWrap: return static_cast<uint8_t>(stored - 1);
  }
  return stored;
}

bool depth_passes(CompareOp op, float z, float stored) {
  switch (op) {
    case CompareOp::Never: return false;
    case CompareOp::Less: return z < stored;
    case CompareOp::Equal: return z == stored;
    case CompareOp::LessEqual: return z <= stored;
    case CompareOp::Greater: return z > stored;
    case CompareOp::NotEqual: return z != stored;
    case CompareOp::GreaterEqual: return z >= stored;
    case CompareOp::Always: return true;
  }
  return false;
}

// One factor of the blend equation for one channel (0 red .. 3 alpha) of the colour `s` and the destination `d`; `k` is the state's constant colour, `s1` the shader's second
// output (dual-source blending).
float blend_factor(BlendFactor f, const float s[4], const float s1[4], const float d[4], const float k[4], uint32_t channel) {
  switch (f) {
    case BlendFactor::Zero: return 0.0f;
    case BlendFactor::One: return 1.0f;
    case BlendFactor::SrcColor: return s[channel];
    case BlendFactor::InvSrcColor: return 1.0f - s[channel];
    case BlendFactor::SrcAlpha: return s[3];
    case BlendFactor::InvSrcAlpha: return 1.0f - s[3];
    case BlendFactor::DstColor: return d[channel];
    case BlendFactor::InvDstColor: return 1.0f - d[channel];
    case BlendFactor::DstAlpha: return d[3];
    case BlendFactor::InvDstAlpha: return 1.0f - d[3];
    case BlendFactor::SrcAlphaSaturate: return channel == 3 ? 1.0f : (s[3] < 1.0f - d[3] ? s[3] : 1.0f - d[3]);
    case BlendFactor::ConstantColor: return k[channel];
    case BlendFactor::InvConstantColor: return 1.0f - k[channel];
    case BlendFactor::ConstantAlpha: return k[3];
    case BlendFactor::InvConstantAlpha: return 1.0f - k[3];
    case BlendFactor::Src1Color: return s1[channel];
    case BlendFactor::InvSrc1Color: return 1.0f - s1[channel];
    case BlendFactor::Src1Alpha: return s1[3];
    case BlendFactor::InvSrc1Alpha: return 1.0f - s1[3];
  }
  return 0.0f;
}

// The pixel after blending `source` into `destination` (both 0xAARRGGBB) by the pipeline's blend state, only the channels of its write mask changed.
uint32_t blend_pixel(const BlendState& b, uint32_t source, uint32_t destination, Vec4 source1) {
  const Vec4 sv = unpack_color(source), dv = unpack_color(destination);
  const float s[4] = {sv.x, sv.y, sv.z, sv.w}, d[4] = {dv.x, dv.y, dv.z, dv.w};
  auto unit = [](float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; };   // (a normalised target's factors are in 0..1)
  const float s1[4] = {unit(source1.x), unit(source1.y), unit(source1.z), unit(source1.w)};
  float out[4];
  for (uint32_t c = 0; c < 4; ++c) {
    const bool alpha = c == 3;
    const BlendOp op = alpha ? b.alpha_op : b.color_op;
    const float sf = blend_factor(alpha ? b.src_alpha : b.src_color, s, s1, d, b.constant, c), df = blend_factor(alpha ? b.dst_alpha : b.dst_color, s, s1, d, b.constant, c);
    if (!b.enable) out[c] = s[c];
    else if (op == BlendOp::Add) out[c] = s[c] * sf + d[c] * df;
    else if (op == BlendOp::Subtract) out[c] = s[c] * sf - d[c] * df;
    else if (op == BlendOp::ReverseSubtract) out[c] = d[c] * df - s[c] * sf;
    else if (op == BlendOp::Min) out[c] = s[c] < d[c] ? s[c] : d[c];
    else out[c] = s[c] > d[c] ? s[c] : d[c];
  }
  const uint32_t blended = pack_color({out[0], out[1], out[2], out[3]});
  uint32_t mask = 0;
  if (b.write_mask & 1) mask |= 0x00FF0000u;
  if (b.write_mask & 2) mask |= 0x0000FF00u;
  if (b.write_mask & 4) mask |= 0x000000FFu;
  if (b.write_mask & 8) mask |= 0xFF000000u;
  return (destination & ~mask) | (blended & mask);
}

// The same for a float target: no clamping anywhere, and `destination` is returned with only the write mask's channels changed.
Vec4 blend_float(const BlendState& b, bool over, Vec4 source, Vec4 destination, Vec4 source1 = {}) {
  const float s[4] = {source.x, source.y, source.z, source.w}, d[4] = {destination.x, destination.y, destination.z, destination.w};
  const float s1[4] = {source1.x, source1.y, source1.z, source1.w};
  float out[4];
  for (uint32_t c = 0; c < 4; ++c) {
    if (!b.custom) {   // (the pipeline's plain source-over, or no blending)
      out[c] = over ? (c == 3 ? s[3] + d[3] * (1.0f - s[3]) : s[c] * s[3] + d[c] * (1.0f - s[3])) : s[c];
      continue;
    }
    const bool alpha = c == 3;
    const BlendOp op = alpha ? b.alpha_op : b.color_op;
    const float sf = blend_factor(alpha ? b.src_alpha : b.src_color, s, s1, d, b.constant, c), df = blend_factor(alpha ? b.dst_alpha : b.dst_color, s, s1, d, b.constant, c);
    if (!b.enable) out[c] = s[c];
    else if (op == BlendOp::Add) out[c] = s[c] * sf + d[c] * df;
    else if (op == BlendOp::Subtract) out[c] = s[c] * sf - d[c] * df;
    else if (op == BlendOp::ReverseSubtract) out[c] = d[c] * df - s[c] * sf;
    else if (op == BlendOp::Min) out[c] = s[c] < d[c] ? s[c] : d[c];
    else out[c] = s[c] > d[c] ? s[c] : d[c];
  }
  const uint32_t mask = b.custom ? b.write_mask : 0xFu;
  return {(mask & 1) ? out[0] : d[0], (mask & 2) ? out[1] : d[1], (mask & 4) ? out[2] : d[2], (mask & 8) ? out[3] : d[3]};
}

// A logic operation on one channel of a wide target: its bits (an integer format's value, a normalised one's quantized number), back as the channel's value.
float logic_channel(LogicOp op, Format format, float source, float destination) {
  const uint32_t bits = format == Format::R8G8B8A8Uint ? 8 : (format == Format::R16G16B16A16Uint || format == Format::R16G16B16A16Sint) ? 16 : 32;
  if (is_integer_format(format)) {
    const uint32_t mask = bits == 32 ? ~0u : (1u << bits) - 1u;
    uint32_t r = apply_logic_op(op, __builtin_bit_cast(uint32_t, source), __builtin_bit_cast(uint32_t, destination)) & mask;
    if ((format == Format::R16G16B16A16Sint) && (r & 0x8000u)) r |= 0xFFFF0000u;   // (a signed channel keeps its sign)
    return __builtin_bit_cast(float, r);
  }
  if (format == Format::R16G16B16A16Unorm) {
    auto q = [](float v) { return static_cast<uint32_t>((v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v) * 65535.0f + 0.5f); };
    return static_cast<float>(apply_logic_op(op, q(source), q(destination)) & 0xFFFFu) * (1.0f / 65535.0f);
  }
  return source;   // (a float target: as if there were no logic operation)
}

// Stencil test, depth write, blend and store of a shaded fragment of the pixel (x, y); depth_ok is the depth test's verdict, `front` whether the primitive is a front face (the stencil
// state of back faces is the pipeline's back_stencil when it has one).
void write_fragment(const DrawState& s, int32_t x, int32_t y, float z, bool depth_ok, const Vec4* colors, uint32_t sample = 0, bool front = true) {
  const GraphicsPipelineDesc& pd = s.pipeline->desc();
  const ArrayView<float> depth = s.depth_plane();
  const ArrayView<uint8_t> stencil = s.stencil_plane();
  const bool stencil_on = pd.stencil.enable && !stencil.empty();
  const uint32_t samples = s.sample_count;
  const uint64_t index = (uint64_t{static_cast<uint32_t>(y)} * s.extent.width + static_cast<uint32_t>(x)) * samples + sample;   // (the sample's place in the depth and stencil planes)
  if (stencil_on) {
    // The stencil test comes first, then the depth test; each outcome has its own op (a fragment the shader discarded changed nothing).
    const StencilState& st = !front && pd.separate_back_stencil ? pd.back_stencil : pd.stencil;
    const uint8_t reference = st.reference;   // (a back face's own with separate_back_stencil)
    const uint8_t stored = stencil[index];
    auto write = [&](StencilOp op) { stencil[index] = static_cast<uint8_t>((stored & ~st.write_mask) | (stencil_apply(op, stored, reference) & st.write_mask)); };
    if (!stencil_compare(st.compare, reference & st.compare_mask, stored & st.compare_mask)) { write(st.fail_op); return; }
    if (!depth_ok) { write(st.depth_fail_op); return; }
    write(st.pass_op);
  } else if (!depth_ok) {
    return;
  }
  if (s.counters) ++s.counters->samples;
  for (uint32_t n = 0; n < pd.color_target_count && n < kMaxColorTargets; ++n) {
    const SoftwareImage* image = n == 0 ? s.color : s.extra_colors[n - 1];
    if (!image) break;
    const Surface surface = s.target(n);
    // (the state of this target: its own with independent blending, which has no source-over shorthand)
    const bool own = pd.independent_blend && n > 0;
    const BlendState& blend = own ? pd.target_blend[n - 1] : pd.blend;
    const bool over = !own && !blend.custom && pd.alpha_blend;
    const uint8_t logic_mask = blend.custom ? blend.write_mask : 0xF;
    if (!surface.floats.empty()) {
      Vec4* pixel = surface.float_row(static_cast<uint32_t>(y)) + x;
      if (pd.logic_op_enable) {
        // (the channels of the shader's colour as the target keeps them, then combined with what is there)
        const Vec4 source = image->store_float(colors[n]), stored = *pixel;
        const Format f = surface.format;
        *pixel = image->store_float({(logic_mask & 1) ? logic_channel(pd.logic_op, f, source.x, stored.x) : stored.x, (logic_mask & 2) ? logic_channel(pd.logic_op, f, source.y, stored.y) : stored.y,
                                     (logic_mask & 4) ? logic_channel(pd.logic_op, f, source.z, stored.z) : stored.z, (logic_mask & 8) ? logic_channel(pd.logic_op, f, source.w, stored.w) : stored.w});
        continue;
      }
      *pixel = image->store_float(blend_float(blend, over, colors[n], *pixel, colors[1]));
      continue;
    }
    uint32_t* pixel = surface.row(static_cast<uint32_t>(y)) + uint64_t{static_cast<uint32_t>(x)} * samples + sample;
    if (is_srgb_format(surface.format)) {
      // Blending is done on linear values: the stored pixel is decoded, blended with the shader's colour, and the result encoded.
      const uint32_t stored = surface.decode(*pixel);
      const float* table = srgb_to_linear_table();
      const Vec4 destination = {table[(stored >> 16) & 0xFF], table[(stored >> 8) & 0xFF], table[stored & 0xFF], static_cast<float>(stored >> 24) * (1.0f / 255.0f)};
      Vec4 linear = pd.logic_op_enable ? colors[n] : blend_float(blend, over, colors[n], destination, colors[1]);   // (an sRGB target: no logic operation)
      if (pd.logic_op_enable) {
        const uint32_t keep = pack_srgb(linear);
        uint32_t mask = 0;
        if (logic_mask & 1) mask |= 0x00FF0000u;
        if (logic_mask & 2) mask |= 0x0000FF00u;
        if (logic_mask & 4) mask |= 0x000000FFu;
        if (logic_mask & 8) mask |= 0xFF000000u;
        *pixel = surface.encode((stored & ~mask) | (keep & mask));
        continue;
      }
      *pixel = surface.encode(pack_srgb(linear));
      continue;
    }
    uint32_t out = pack_color(colors[n]);
    if (pd.logic_op_enable) {
      // (the bytes of the target: each channel's 8 bits)
      const uint32_t stored = surface.decode(*pixel);
      uint32_t mask = 0;
      if (logic_mask & 1) mask |= 0x00FF0000u;
      if (logic_mask & 2) mask |= 0x0000FF00u;
      if (logic_mask & 4) mask |= 0x000000FFu;
      if (logic_mask & 8) mask |= 0xFF000000u;
      *pixel = surface.encode((stored & ~mask) | (apply_logic_op(pd.logic_op, out, stored) & mask));
      continue;
    }
    if (blend.custom) out = blend_pixel(blend, out, surface.decode(*pixel), colors[1]);
    else if (over) out = blend_over(out, surface.decode(*pixel));
    *pixel = surface.encode(out);
  }
  if (pd.depth_write && !depth.empty()) depth[index] = z;
}

// Runs the pipeline's fragment shader; false if it discarded the pixel.
bool run_fragment_shader(const DrawState& s, const Vec4* varying, const FragmentContext& given, Vec4* color) {
  s.count(PipelineStatistic::PsInvocations);
  FragmentContext context = given;
  context.storage = s.storage;
  context.images = s.images;
  if (s.pipeline->fragment_ex()) return s.pipeline->fragment_ex()(varying, context, s.constants, s.textures, color);
  if (s.pipeline->textured()) return s.pipeline->textured()(varying, s.constants, s.textures, color);
  return s.pipeline->fragment()(varying, s.constants, color);
}

// Where the samples of a pixel are, in sixteenths of a pixel from its centre (+y down): the standard patterns of Direct3D 10.
struct SamplePosition {
  int8_t x, y;
};
constexpr SamplePosition kSamples2[2] = {{4, 4}, {-4, -4}};
constexpr SamplePosition kSamples4[4] = {{-2, -6}, {6, -2}, {-6, 2}, {2, 6}};
constexpr SamplePosition kSamples8[8] = {{1, -3}, {-1, 3}, {5, 1}, {-3, -5}, {-5, 5}, {-7, -1}, {3, 7}, {7, -7}};
inline const SamplePosition* sample_positions(uint32_t count) { return count == 2 ? kSamples2 : count == 4 ? kSamples4 : kSamples8; }

// An edge is "top" or "left" when pixels exactly on it belong to the triangle.
inline bool owns_edge(float dx, float dy) { return dy < 0 || (dy == 0 && dx > 0); }

void raster_line(const DrawState& s, const ScreenVertex& v0, const ScreenVertex& v1, float bias, bool front);

// Half of a quadrilateral line (LineMode::QuadrilateralWide / Narrow) given to raster_triangle: no culling, filled whatever the fill mode, not conservative, of the line's facing and depth bias.
struct LineQuad {
  float bias;
  bool front;
};

// `outline` says which edges of the triangle are edges of the primitive (bit 0 a-b, bit 1 b-c, bit 2 c-a): what a wireframe fill draws.
void raster_triangle(const DrawState& s, const ScreenVertex& a, const ScreenVertex& b, const ScreenVertex& c, uint8_t outline, const LineQuad* line = nullptr) {
  const GraphicsPipelineDesc& pd = s.pipeline->desc();
  float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);  // y down: > 0 is clockwise on screen
  if (area == 0) return;
  const bool clockwise = area > 0;
  const bool front = line ? line->front : (pd.front_face == FrontFace::Clockwise) == clockwise;
  if (!line && pd.cull_mode == CullMode::Back && !front) return;
  if (!line && pd.cull_mode == CullMode::Front && front) return;

  // The depth bias of the triangle: from the slope of its plane of window depths and its largest depth.
  float bias = line ? line->bias : 0.0f;
  if (!line && pd.depth_bias.enabled()) {
    const float dzdx = ((b.z - a.z) * (c.y - a.y) - (c.z - a.z) * (b.y - a.y)) / area, dzdy = ((c.z - a.z) * (b.x - a.x) - (b.z - a.z) * (c.x - a.x)) / area;
    const float sx = dzdx < 0 ? -dzdx : dzdx, sy = dzdy < 0 ? -dzdy : dzdy;
    float max_z = a.z > b.z ? a.z : b.z;
    if (c.z > max_z) max_z = c.z;
    bias = depth_bias_value(pd.depth_bias, sx > sy ? sx : sy, max_z);
  }
  if (!line && pd.fill_mode == FillMode::Wireframe) {
    if (outline & 1) raster_line(s, a, b, bias, front);
    if (outline & 2) raster_line(s, b, c, bias, front);
    if (outline & 4) raster_line(s, c, a, bias, front);
    return;
  }
  const float depth_lo = s.viewport.min_depth < s.viewport.max_depth ? s.viewport.min_depth : s.viewport.max_depth;
  const float depth_hi = s.viewport.min_depth < s.viewport.max_depth ? s.viewport.max_depth : s.viewport.min_depth;
  // (a fragment's depth: the plane's, biased, within the viewport's range)
  auto final_z = [&](float z) {
    z += bias;
    return z < depth_lo ? depth_lo : z > depth_hi ? depth_hi : z;
  };

  // Work with a positive area so inside tests are "edge >= 0".
  const ScreenVertex* v0 = &a;
  const ScreenVertex* v1 = &b;
  const ScreenVertex* v2 = &c;
  if (area < 0) {
    v1 = &c;
    v2 = &b;
    area = -area;
  }

  int32_t min_x = static_cast<int32_t>(v0->x), max_x = min_x, min_y = static_cast<int32_t>(v0->y), max_y = min_y;
  const ScreenVertex* verts[3] = {v0, v1, v2};
  float fx0 = v0->x, fx1 = v0->x, fy0 = v0->y, fy1 = v0->y;
  for (const ScreenVertex* v : verts) {
    if (v->x < fx0) fx0 = v->x;
    if (v->x > fx1) fx1 = v->x;
    if (v->y < fy0) fy0 = v->y;
    if (v->y > fy1) fy1 = v->y;
  }
  const int64_t sx0 = s.scissor.x, sy0 = s.scissor.y;
  const int64_t sx1 = sx0 + s.scissor.width, sy1 = sy0 + s.scissor.height;
  auto clamp_floor = [](float v, int64_t lo, int64_t hi) {
    if (!(v > static_cast<float>(lo))) return lo;
    if (v >= static_cast<float>(hi)) return hi;
    return static_cast<int64_t>(v);
  };
  min_x = static_cast<int32_t>(clamp_floor(fx0 - 0.5f, sx0, sx1));
  max_x = static_cast<int32_t>(clamp_floor(fx1 + 1.5f, sx0, sx1));
  min_y = static_cast<int32_t>(clamp_floor(fy0 - 0.5f, sy0, sy1));
  max_y = static_cast<int32_t>(clamp_floor(fy1 + 1.5f, sy0, sy1));

  const float e0dx = v2->x - v1->x, e0dy = v2->y - v1->y;  // edge opposite v0
  const float e1dx = v0->x - v2->x, e1dy = v0->y - v2->y;  // opposite v1
  const float e2dx = v1->x - v0->x, e2dy = v1->y - v0->y;  // opposite v2
  const bool own0 = owns_edge(e0dx, e0dy), own1 = owns_edge(e1dx, e1dy), own2 = owns_edge(e2dx, e2dy);
  const float inv_area = 1.0f / area;
  const ArrayView<float> depth = s.depth_plane();
  const ArrayView<uint8_t> stencil = s.stencil_plane();
  const bool stencil_on = pd.stencil.enable && !stencil.empty();
  // The varyings at any point of the plane of the triangle (also outside it, as the hardware extends them to the pixels of a quad it only partly covers).
  auto varyings_at = [&](float qx, float qy, Vec4* out) {
    const float l0 = -((qx - v1->x) * e0dy - (qy - v1->y) * e0dx), l1 = -((qx - v2->x) * e1dy - (qy - v2->y) * e1dx), l2 = -((qx - v0->x) * e2dy - (qy - v0->y) * e2dx);
    const float p0 = l0 * inv_area * v0->inv_w, p1 = l1 * inv_area * v1->inv_w, p2 = l2 * inv_area * v2->inv_w;
    const float norm = 1.0f / (p0 + p1 + p2);
    for (uint32_t i = 0; i < pd.varying_count; ++i)
      out[i] = {(v0->varying[i].x * p0 + v1->varying[i].x * p1 + v2->varying[i].x * p2) * norm, (v0->varying[i].y * p0 + v1->varying[i].y * p1 + v2->varying[i].y * p2) * norm,
                (v0->varying[i].z * p0 + v1->varying[i].z * p1 + v2->varying[i].z * p2) * norm, (v0->varying[i].w * p0 + v1->varying[i].w * p1 + v2->varying[i].w * p2) * norm};
  };
  const bool lod_pass = s.pipeline->samples_textures() && s.textures.uses_lod();

  // Per row, the columns that can be inside: each edge value is linear in x, so it is solved for where it turns non-negative and the range is widened by two pixels, which covers the
  // rounding. The exact tests below still decide every pixel, so the picture is the one a full scan of the bounding box gives. (Far off-screen vertices make the solving imprecise: no trimming.)
  // Conservative rasterization: a pixel is covered when its square and the triangle overlap. By the separating axis theorem that is when the square meets the triangle's bounding box
  // and no edge has the whole square outside: an edge value is linear, so its largest over the square is the value at the centre plus half the sum of its gradient's magnitudes.
  const bool conservative = pd.conservative && !line;
  auto magnitude = [](float v) { return v < 0 ? -v : v; };
  const float reach0 = 0.5f * (magnitude(e0dx) + magnitude(e0dy)), reach1 = 0.5f * (magnitude(e1dx) + magnitude(e1dy)), reach2 = 0.5f * (magnitude(e2dx) + magnitude(e2dy));
  const bool trim = !conservative && fx0 > -1e5f && fx1 < 1e5f && fy0 > -1e5f && fy1 < 1e5f;
  struct EdgeLine {
    float vx, vy, dx, dy;
  };
  const EdgeLine edges[3] = {{v1->x, v1->y, e0dx, e0dy}, {v2->x, v2->y, e1dx, e1dy}, {v0->x, v0->y, e2dx, e2dy}};
  const uint32_t target_width = s.extent.width;

  for (int32_t y = min_y; y < max_y; ++y) {
    int32_t row_begin = min_x, row_end = max_x;
    if (trim) {
      const float row_y = y + 0.5f;
      bool empty = false;
      for (const EdgeLine& e : edges) {
        const float t = (row_y - e.vy) * e.dx;   // (the edge value is t - (px - vx) * dy, to be >= 0)
        if (e.dy == 0) {
          if (t < 0) empty = true;
        } else {
          const float bound = e.vx + t / e.dy - 0.5f;   // (the x whose pixel centre has the value zero)
          if (e.dy < 0) {   // the value grows with x: it is non-negative from the bound on
            const float lo = bound - 2.0f;
            if (lo >= static_cast<float>(row_end)) empty = true;
            else if (lo > static_cast<float>(row_begin)) row_begin = static_cast<int32_t>(lo);
          } else {   // it shrinks with x: non-negative up to the bound
            const float hi = bound + 3.0f;
            if (hi <= static_cast<float>(row_begin)) empty = true;
            else if (hi < static_cast<float>(row_end)) row_end = static_cast<int32_t>(hi);
          }
        }
      }
      if (empty || row_begin >= row_end) continue;
    }
    for (int32_t x = row_begin; x < row_end; ++x) {
      const float px = x + 0.5f, py = y + 0.5f;
      const float w0 = (px - v1->x) * e0dy - (py - v1->y) * e0dx;
      const float w1 = (px - v2->x) * e1dy - (py - v2->y) * e1dx;
      const float w2 = (px - v0->x) * e2dy - (py - v0->y) * e2dx;
      // With y down and a positive area, an interior point has all three edge values <= 0 in this
      // formulation, so flip the sign to get weights.
      const float l0 = -w0, l1 = -w1, l2 = -w2;
      const uint32_t sample_count = s.sample_count;
      // Which samples the triangle covers, and the depth at each (a pixel with one sample per pixel is the centre; a multisampled pass without multisample covers all or none by the centre).
      uint32_t covered = 0;
      float sample_z[8];
      auto weights_inside = [&](float a0, float a1, float a2) {
        if (a0 < 0 || a1 < 0 || a2 < 0) return false;
        return !((a0 == 0 && !own0) || (a1 == 0 && !own1) || (a2 == 0 && !own2));
      };
      if (conservative) {
        const float fx = static_cast<float>(x), fy = static_cast<float>(y);
        if (fx > fx1 || fx + 1.0f < fx0 || fy > fy1 || fy + 1.0f < fy0) continue;
        if (l0 + reach0 < 0 || l1 + reach1 < 0 || l2 + reach2 < 0) continue;
        covered = (1u << sample_count) - 1;   // (every sample of a touched pixel, at the plane's depth at the centre)
        const float centre_z = (l0 * v0->z + l1 * v1->z + l2 * v2->z) * inv_area;
        for (uint32_t k = 0; k < sample_count; ++k) sample_z[k] = centre_z;
      } else if (sample_count > 1 && pd.multisample) {
        const SamplePosition* positions = sample_positions(sample_count);
        for (uint32_t k = 0; k < sample_count; ++k) {
          const float sx = px + static_cast<float>(positions[k].x) * (1.0f / 16.0f), sy = py + static_cast<float>(positions[k].y) * (1.0f / 16.0f);
          const float a0 = -((sx - v1->x) * e0dy - (sy - v1->y) * e0dx), a1 = -((sx - v2->x) * e1dy - (sy - v2->y) * e1dx), a2 = -((sx - v0->x) * e2dy - (sy - v0->y) * e2dx);
          if (!weights_inside(a0, a1, a2)) continue;
          covered |= 1u << k;
          sample_z[k] = final_z((a0 * v0->z + a1 * v1->z + a2 * v2->z) * inv_area);
        }
        if (!covered) continue;
      } else {
        if (!weights_inside(l0, l1, l2)) continue;
        covered = (1u << sample_count) - 1;
        const float centre_z = final_z((l0 * v0->z + l1 * v1->z + l2 * v2->z) * inv_area);
        for (uint32_t k = 0; k < sample_count; ++k) sample_z[k] = centre_z;
      }
      const float b0 = l0 * inv_area, b1 = l1 * inv_area, b2 = l2 * inv_area;

      const uint64_t index = uint64_t{static_cast<uint32_t>(y)} * target_width + static_cast<uint32_t>(x);
      const float z = final_z(b0 * v0->z + b1 * v1->z + b2 * v2->z);
      uint32_t depth_pass = covered;   // (the samples that pass the depth test)
      if (pd.depth_test && !depth.empty())
        for (uint32_t k = 0; k < sample_count; ++k)
          if ((covered >> k & 1) && !depth_passes(pd.depth_compare, sample_z[k], depth[index * sample_count + k])) depth_pass &= ~(1u << k);
      if (!depth_pass && !stencil_on && !pd.fragment_stores) continue;   // (with a stencil test the depth result is needed after the shader; a shader that writes buffers runs whatever the depth test says)

      // Perspective-correct varyings: weights scaled by 1/w.
      const float p0 = b0 * v0->inv_w, p1 = b1 * v1->inv_w, p2 = b2 * v2->inv_w;
      const float norm = 1.0f / (p0 + p1 + p2);
      Vec4 varying[kMaxVaryings];
      for (uint32_t i = 0; i < pd.varying_count; ++i) {
        varying[i] = {(v0->varying[i].x * p0 + v1->varying[i].x * p1 + v2->varying[i].x * p2) * norm,
                      (v0->varying[i].y * p0 + v1->varying[i].y * p1 + v2->varying[i].y * p2) * norm,
                      (v0->varying[i].z * p0 + v1->varying[i].z * p1 + v2->varying[i].z * p2) * norm,
                      (v0->varying[i].w * p0 + v1->varying[i].w * p1 + v2->varying[i].w * p2) * norm};
      }
      Vec4 color[kMaxColorTargets];
      if (lod_pass) {
        // The level of detail comes from the 2 x 2 quad the pixel is in: the shader is run for the quad's upper left, right and lower pixels with the lookups only recorded.
        const int32_t qx = x & ~1, qy = y & ~1;
        const float xs[3] = {static_cast<float>(qx) + 0.5f, static_cast<float>(qx) + 1.5f, static_cast<float>(qx) + 0.5f};
        const float ys[3] = {static_cast<float>(qy) + 0.5f, static_cast<float>(qy) + 0.5f, static_cast<float>(qy) + 1.5f};
        float coord[3][kMaxTextureUnits][3] = {};
        bool have[3][kMaxTextureUnits] = {};
        for (int k = 0; k < 3; ++k) {
          Vec4 quad_varying[kMaxVaryings];
          varyings_at(xs[k], ys[k], quad_varying);
          Vec4 ignored[kMaxColorTargets];
          FragmentContext quad_context;
          quad_context.position = {xs[k], ys[k], z, 1.0f};
          quad_context.front_facing = front;
          quad_context.primitive_id = s.primitive_id;
          quad_context.helper = true;
          s.textures.start_recording();
          run_fragment_shader(s, quad_varying, quad_context, ignored);
          s.textures.stop_recording();
          for (uint32_t unit = 0; unit < kMaxTextureUnits; ++unit) have[k][unit] = s.textures.recorded(unit, coord[k][unit]);
        }
        s.textures.clear_lod();
        for (uint32_t unit = 0; unit < kMaxTextureUnits; ++unit)
          if (have[0][unit] && have[1][unit] && have[2][unit]) s.textures.set_lod(unit, coord[0][unit], coord[1][unit], coord[2][unit]);
      }
      FragmentContext context;
      context.position = {px, py, z, b0 * v0->inv_w + b1 * v1->inv_w + b2 * v2->inv_w};
      context.front_facing = front;
      context.primitive_id = s.primitive_id;
      // (the corners of the pixel's quad for the derivatives: the varyings, and depth and 1 / w, of the plane of the triangle at the corner's centre)
      auto quad_source = [&](uint32_t corner, Vec4* out_varyings, Vec4* out_position) {
        const float qx = static_cast<float>(x & ~1) + 0.5f + static_cast<float>(corner & 1), qy = static_cast<float>(y & ~1) + 0.5f + static_cast<float>(corner >> 1);
        varyings_at(qx, qy, out_varyings);
        const float l0 = -((qx - v1->x) * e0dy - (qy - v1->y) * e0dx), l1 = -((qx - v2->x) * e1dy - (qy - v2->y) * e1dx), l2 = -((qx - v0->x) * e2dy - (qy - v0->y) * e2dx);
        const float a0 = l0 * inv_area, a1 = l1 * inv_area, a2 = l2 * inv_area;
        *out_position = {qx, qy, final_z(a0 * v0->z + a1 * v1->z + a2 * v2->z), a0 * v0->inv_w + a1 * v1->inv_w + a2 * v2->inv_w};
      };
      using QuadSource = decltype(quad_source);
      context.quad_fn = [](const void* user, uint32_t corner, Vec4* out_varyings, Vec4* out_position) { (*static_cast<const QuadSource*>(user))(corner, out_varyings, out_position); };
      context.quad_user = &quad_source;
      context.quad_corner = static_cast<uint32_t>(x & 1) | static_cast<uint32_t>(y & 1) << 1;
      if (!run_fragment_shader(s, varying, context, color)) continue;
      if (pd.alpha_to_coverage && pd.multisample && sample_count > 1) {
        // (the first round(alpha * samples) samples of the pixel keep what they covered)
        const float a = color[0].w;
        uint32_t keep = !(a > 0.0f) ? 0 : a >= 1.0f ? sample_count : static_cast<uint32_t>(a * static_cast<float>(sample_count) + 0.5f);
        if (keep > sample_count) keep = sample_count;
        covered &= (1u << keep) - 1;
      }
      for (uint32_t k = 0; k < sample_count; ++k)
        if (covered >> k & 1) write_fragment(s, x, y, sample_z[k], (depth_pass >> k & 1) != 0, color, k, front);
    }
  }
}

// A screen-space point or line fragment: the same shader, depth, stencil and blend as a triangle's, from varyings interpolated perspective-correct along the line.
void fragment_at(const DrawState& s, int32_t x, int32_t y, float z, float inv_w, const Vec4* varying, bool front = true, float coverage = 1.0f) {
  const GraphicsPipelineDesc& pd = s.pipeline->desc();
  {
    const float lo = s.viewport.min_depth < s.viewport.max_depth ? s.viewport.min_depth : s.viewport.max_depth, hi = s.viewport.min_depth < s.viewport.max_depth ? s.viewport.max_depth : s.viewport.min_depth;
    z = z < lo ? lo : z > hi ? hi : z;   // (the viewport's depth range)
  }
  const Surface target = s.target(0);
  if (x < s.scissor.x || y < s.scissor.y || x >= static_cast<int64_t>(s.scissor.x) + s.scissor.width || y >= static_cast<int64_t>(s.scissor.y) + s.scissor.height) return;
  if (x < 0 || y < 0 || static_cast<uint32_t>(x) >= target.width || static_cast<uint32_t>(y) >= target.height) return;
  const ArrayView<float> depth = s.depth_plane();
  const uint64_t index = uint64_t{static_cast<uint32_t>(y)} * s.extent.width + static_cast<uint32_t>(x);
  const uint32_t sample_count = s.sample_count;   // (a line or point covers every sample of its pixel)
  uint32_t depth_pass = (1u << sample_count) - 1;
  if (pd.depth_test && !depth.empty())
    for (uint32_t k = 0; k < sample_count; ++k)
      if (!depth_passes(pd.depth_compare, z, depth[index * sample_count + k])) depth_pass &= ~(1u << k);
  if (!depth_pass && !(pd.stencil.enable && !s.stencil_plane().empty())) return;
  FragmentContext context;
  context.position = {static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f, z, inv_w};
  context.front_facing = front;
  context.primitive_id = s.primitive_id;
  Vec4 color[kMaxColorTargets];
  if (!run_fragment_shader(s, varying, context, color)) return;
  color[0].w *= coverage;   // (an alpha-antialiased line's part of the pixel, into the first target's alpha before blending)
  for (uint32_t k = 0; k < sample_count; ++k) write_fragment(s, x, y, z, (depth_pass >> k & 1) != 0, color, k, front);
}

ScreenVertex to_screen(const DrawState& s, const ClipVertex& v, uint32_t varyings) {
  ScreenVertex sv;
  const float inv_w = 1.0f / v.position.w;
  sv.x = s.viewport.x + (v.position.x * inv_w * 0.5f + 0.5f) * s.viewport.width;
  sv.y = s.viewport.y + (v.position.y * inv_w * 0.5f + 0.5f) * s.viewport.height;
  sv.z = s.viewport.min_depth + v.position.z * inv_w * (s.viewport.max_depth - s.viewport.min_depth);
  sv.inv_w = inv_w;
  for (uint32_t k = 0; k < varyings; ++k) sv.varying[k] = v.varying[k];
  return sv;
}

// The clipper's statistics of a primitive of `count` vertices: it was given it (ClipperInvocations), and it came out of it unless it lies wholly outside one plane of the view volume
// (-w <= x <= w, -w <= y <= w, 0 <= z <= w), which is all a clipper throws away.
void count_clipped(const DrawState& s, const ClipVertex* v, uint32_t count) {
  if (!s.counters) return;
  s.count(PipelineStatistic::ClipperInvocations);
  uint32_t outside = 0x3F;   // (the planes every vertex is outside of)
  for (uint32_t k = 0; k < count; ++k) {
    const Vec4& p = v[k].position;
    outside &= (p.x < -p.w ? 1u : 0u) | (p.x > p.w ? 2u : 0u) | (p.y < -p.w ? 4u : 0u) | (p.y > p.w ? 8u : 0u) | (p.z < 0 ? 16u : 0u) | (p.z > p.w ? 32u : 0u);
  }
  if (!outside) s.count(PipelineStatistic::ClipperPrimitives);
}

// One pixel for a point (the pixel its position falls in).
void draw_point(const DrawState& s, const ClipVertex& a) {
  const uint32_t varyings = s.pipeline->desc().varying_count;
  count_clipped(s, &a, 1);
  s.select_layer(a.layer);
  s.select_viewport(a.viewport);
  if (!(a.position.w > 1e-9f)) return;
  if (s.pipeline->desc().depth_clip && (a.position.z < 0 || a.position.z > a.position.w)) return;   // (outside the near or far plane)
  const ScreenVertex v = to_screen(s, a, varyings);
  if (!(v.x > -1e6f && v.x < 1e6f && v.y > -1e6f && v.y < 1e6f)) return;
  const int32_t x = static_cast<int32_t>(v.x < 0 ? v.x - 1 : v.x), y = static_cast<int32_t>(v.y < 0 ? v.y - 1 : v.y);   // (floor, as far as the pixel it falls in goes)
  fragment_at(s, x, y, v.z, v.inv_w, v.varying);
}

// A line one pixel wide: a step per pixel along the longer axis, the other coordinate and the varyings interpolated (perspective-correct) along it.
// The planes a primitive is clipped against: with depth clipping the near (z >= 0) and far (z <= w) planes, without it only w >= a little above 0 (what the division by w needs).
uint32_t clip_plane_count(const DrawState& s) { return s.pipeline->desc().depth_clip ? 2 : 1; }
float clip_distance(const DrawState& s, uint32_t plane, const Vec4& p) {
  if (!s.pipeline->desc().depth_clip) return p.w - 1e-6f;
  return plane == 0 ? p.z : p.w - p.z;
}

void draw_line(const DrawState& s, const ClipVertex& a, const ClipVertex& b) {
  const uint32_t varyings = s.pipeline->desc().varying_count;
  s.select_layer(a.layer);
  s.select_viewport(a.viewport);
  ClipVertex p = a, q = b;
  const ClipVertex ends[2] = {a, b};
  count_clipped(s, ends, 2);
  for (uint32_t plane = 0; plane < clip_plane_count(s); ++plane) {
    const float dp = clip_distance(s, plane, p.position), dq = clip_distance(s, plane, q.position);
    if (dp < 0 && dq < 0) return;
    if (dp < 0) p = mix(p, q, dp / (dp - dq), varyings);
    else if (dq < 0) q = mix(p, q, dp / (dp - dq), varyings);
  }
  if (!(p.position.w > 1e-9f) || !(q.position.w > 1e-9f)) return;
  raster_line(s, to_screen(s, p, varyings), to_screen(s, q, varyings), 0.0f, true);
}

// A quadrilateral line (LineMode::QuadrilateralWide, Narrow): the rectangle `width` pixels wide whose middle line runs from v0 to v1, as two triangles (the samples, or the pixel centres,
// it covers; its corners take the attributes of their end).
void raster_quad_line(const DrawState& s, const ScreenVertex& v0, const ScreenVertex& v1, float width, float bias, bool front) {
  const float dx = v1.x - v0.x, dy = v1.y - v0.y;
  const float length = __builtin_sqrtf(dx * dx + dy * dy);
  if (!(length > 0.0f)) return;   // (no area)
  const float nx = -dy / length * (width * 0.5f), ny = dx / length * (width * 0.5f);
  ScreenVertex a0 = v0, a1 = v0, b0 = v1, b1 = v1;
  a0.x += nx, a0.y += ny, a1.x -= nx, a1.y -= ny;
  b0.x += nx, b0.y += ny, b1.x -= nx, b1.y -= ny;
  const LineQuad line{bias, front};
  raster_triangle(s, a0, a1, b1, 0, &line);
  raster_triangle(s, a0, b1, b0, 0, &line);
}

// An alpha-antialiased line (LineMode::AlphaAntialiased): the rectangle one pixel wide between v0 and v1 (its ends at the two points, not lengthened). Every pixel it touches is shaded
// once, at its centre with the attributes of the nearest point of the line, and the first target's alpha is multiplied by the part of the pixel the rectangle covers (measured on
// 8 x 8 points of the pixel).
void raster_smooth_line(const DrawState& s, const ScreenVertex& v0, const ScreenVertex& v1, float bias, bool front) {
  const uint32_t varyings = s.pipeline->desc().varying_count;
  const float dx = v1.x - v0.x, dy = v1.y - v0.y;
  const float length = __builtin_sqrtf(dx * dx + dy * dy);
  if (!(length > 0.0f)) return;
  const float ux = dx / length, uy = dy / length;   // (along the line; across it: (-uy, ux))
  // (the pixels around the rectangle, within the scissor rectangle)
  auto lo = [](float a, float b) { return a < b ? a : b; };
  auto hi = [](float a, float b) { return a > b ? a : b; };
  const float fx0 = lo(v0.x, v1.x) - 1.0f, fx1 = hi(v0.x, v1.x) + 1.0f, fy0 = lo(v0.y, v1.y) - 1.0f, fy1 = hi(v0.y, v1.y) + 1.0f;
  const int64_t sx0 = s.scissor.x, sy0 = s.scissor.y, sx1 = sx0 + s.scissor.width, sy1 = sy0 + s.scissor.height;
  auto clamp_floor = [](float v, int64_t a, int64_t b) {
    if (!(v > static_cast<float>(a))) return a;
    if (v >= static_cast<float>(b)) return b;
    return static_cast<int64_t>(v);
  };
  const int64_t x_begin = clamp_floor(fx0, sx0, sx1), x_end = clamp_floor(fx1 + 1.0f, sx0, sx1), y_begin = clamp_floor(fy0, sy0, sy1), y_end = clamp_floor(fy1 + 1.0f, sy0, sy1);
  constexpr uint32_t kGrid = 8;
  for (int64_t y = y_begin; y < y_end; ++y) {
    for (int64_t x = x_begin; x < x_end; ++x) {
      uint32_t inside = 0;
      for (uint32_t j = 0; j < kGrid; ++j)
        for (uint32_t i = 0; i < kGrid; ++i) {
          const float px = static_cast<float>(x) + (static_cast<float>(i) + 0.5f) / kGrid - v0.x, py = static_cast<float>(y) + (static_cast<float>(j) + 0.5f) / kGrid - v0.y;
          const float along = px * ux + py * uy, across = -px * uy + py * ux;
          if (along >= 0.0f && along <= length && across >= -0.5f && across <= 0.5f) ++inside;
        }
      if (!inside) continue;
      const float cx = static_cast<float>(x) + 0.5f - v0.x, cy = static_cast<float>(y) + 0.5f - v0.y;
      float t = (cx * ux + cy * uy) / length;
      t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
      const float p0 = (1.0f - t) * v0.inv_w, p1 = t * v1.inv_w;
      const float norm = 1.0f / (p0 + p1);
      Vec4 varying[kMaxVaryings];
      for (uint32_t k = 0; k < varyings; ++k)
        varying[k] = {(v0.varying[k].x * p0 + v1.varying[k].x * p1) * norm, (v0.varying[k].y * p0 + v1.varying[k].y * p1) * norm, (v0.varying[k].z * p0 + v1.varying[k].z * p1) * norm,
                      (v0.varying[k].w * p0 + v1.varying[k].w * p1) * norm};
      const float z = v0.z + (v1.z - v0.z) * t;
      fragment_at(s, static_cast<int32_t>(x), static_cast<int32_t>(y), z + bias, p0 + p1, varying, front, static_cast<float>(inside) / static_cast<float>(kGrid * kGrid));
    }
  }
}

// The pixels of a line between two window-space points, its depth biased by `bias`; `front` is what the fragments are (a wireframe triangle's facing).
void raster_line(const DrawState& s, const ScreenVertex& v0, const ScreenVertex& v1, float bias, bool front) {
  const uint32_t varyings = s.pipeline->desc().varying_count;
  const float limit = 1e6f;
  if (!(v0.x > -limit && v0.x < limit && v0.y > -limit && v0.y < limit && v1.x > -limit && v1.x < limit && v1.y > -limit && v1.y < limit)) return;
  const LineMode mode = s.pipeline->desc().line_mode;
  if (mode == LineMode::QuadrilateralWide || mode == LineMode::QuadrilateralNarrow) return raster_quad_line(s, v0, v1, mode == LineMode::QuadrilateralWide ? 1.4f : 1.0f, bias, front);
  if (mode == LineMode::AlphaAntialiased) return raster_smooth_line(s, v0, v1, bias, front);
  const float dx = v1.x - v0.x, dy = v1.y - v0.y;
  const float length = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy) ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
  // (the steps are clamped to the target: a line past it costs no more than the target's size)
  uint32_t steps = length < 1.0f ? 1u : (length > 16384.0f ? 16384u : static_cast<uint32_t>(length + 0.999f));
  for (uint32_t i = 0; i <= steps; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(steps);
    const float x = v0.x + dx * t, y = v0.y + dy * t, z = v0.z + (v1.z - v0.z) * t;
    const float p0 = (1.0f - t) * v0.inv_w, p1 = t * v1.inv_w;
    const float norm = 1.0f / (p0 + p1);
    Vec4 varying[kMaxVaryings];
    for (uint32_t k = 0; k < varyings; ++k)
      varying[k] = {(v0.varying[k].x * p0 + v1.varying[k].x * p1) * norm, (v0.varying[k].y * p0 + v1.varying[k].y * p1) * norm, (v0.varying[k].z * p0 + v1.varying[k].z * p1) * norm,
                    (v0.varying[k].w * p0 + v1.varying[k].w * p1) * norm};
    fragment_at(s, static_cast<int32_t>(x < 0 ? x - 1 : x), static_cast<int32_t>(y < 0 ? y - 1 : y), z + bias, (1.0f - t) * v0.inv_w + t * v1.inv_w, varying, front);
  }
}

// Clips a clip-space triangle against the planes of clip_distance (the near and far planes, or w > 0 without depth clipping), divides by w and rasterizes the polygon that
// remains as a fan of triangles.
void draw_triangle(const DrawState& s, const ClipVertex& a, const ClipVertex& b, const ClipVertex& c) {
  const uint32_t varyings = s.pipeline->desc().varying_count;
  s.select_layer(a.layer);
  s.select_viewport(a.viewport);
  constexpr uint32_t kMaxPolygon = 6;   // (a triangle cut by two planes has at most five corners)
  ClipVertex polys[2][kMaxPolygon];
  {
    const ClipVertex corners[3] = {a, b, c};
    count_clipped(s, corners, 3);
  }
  polys[0][0] = a, polys[0][1] = b, polys[0][2] = c;
  uint32_t n = 3, current = 0;
  for (uint32_t plane = 0; plane < clip_plane_count(s); ++plane) {
    const ClipVertex* in = polys[current];
    ClipVertex* out = polys[current ^ 1];
    uint32_t m = 0;
    for (uint32_t i = 0; i < n; ++i) {
      const ClipVertex& cur = in[i];
      const ClipVertex& next = in[(i + 1) % n];
      const float dc = clip_distance(s, plane, cur.position), dn = clip_distance(s, plane, next.position);
      const bool cur_in = dc >= 0, next_in = dn >= 0;
      if (cur_in && m < kMaxPolygon) out[m++] = cur;
      if (cur_in != next_in && m < kMaxPolygon) out[m++] = mix(cur, next, dc / (dc - dn), varyings);
    }
    n = m;
    current ^= 1;
    if (n < 3) return;
  }
  const ClipVertex* poly = polys[current];

  ScreenVertex sv[kMaxPolygon];
  for (uint32_t i = 0; i < n; ++i) {
    const Vec4& p = poly[i].position;
    if (!(p.w > 1e-9f)) return;  // behind the eye even after clipping: nothing sensible to draw
    const float inv_w = 1.0f / p.w;
    sv[i].x = s.viewport.x + (p.x * inv_w * 0.5f + 0.5f) * s.viewport.width;
    sv[i].y = s.viewport.y + (p.y * inv_w * 0.5f + 0.5f) * s.viewport.height;
    sv[i].z = s.viewport.min_depth + p.z * inv_w * (s.viewport.max_depth - s.viewport.min_depth);
    sv[i].inv_w = inv_w;
    for (uint32_t k = 0; k < varyings; ++k) sv[i].varying[k] = poly[i].varying[k];
  }
  // (the fan's inner edges are not edges of the polygon: a wireframe fill leaves them out)
  for (uint32_t i = 1; i + 1 < n; ++i) raster_triangle(s, sv[0], sv[i], sv[i + 1], static_cast<uint8_t>((i == 1 ? 1u : 0u) | 2u | (i + 2 == n ? 4u : 0u)));
}

// The vertex buffers a draw reads: what is bound to each slot and where the data starts.
struct VertexStreams {
  ConstByteView data[kMaxVertexBuffers];
  uint64_t offset[kMaxVertexBuffers] = {};
};

// `vertex` is the vertex's number in the buffers; a per-instance attribute reads the element of instance `instance` (from 0) of a draw from `first_instance`
// (instance_element: its step rate divides the instance).
ClipVertex run_vertex_shader(const SoftwarePipeline& pipeline, const VertexStreams& streams, uint64_t vertex, uint32_t first_instance, uint32_t instance,
                             const uint8_t* constants, const VertexContext& context) {
  const GraphicsPipelineDesc& pd = pipeline.desc();
  Vec4 attributes[kMaxVertexAttributes];
  for (Vec4& a : attributes) a = {0, 0, 0, 1};
  for (uint32_t i = 0; i < pd.attribute_count; ++i) {
    const VertexAttribute& at = pd.attributes[i];
    if (at.location >= kMaxVertexAttributes) continue;
    const uint32_t slot = at.slot < kMaxVertexBuffers ? at.slot : 0;
    const uint32_t stride = slot == 0 ? pd.vertex_stride : pd.slot_strides[slot];
    const uint64_t base = streams.offset[slot] + (at.per_instance ? instance_element(at, first_instance, instance) : vertex) * stride;
    attributes[at.location] = read_vertex_attribute(streams.data[slot], base + at.offset, at.format);
  }
  ShaderIo io;
  if (pipeline.vertex_ex()) pipeline.vertex_ex()(attributes, constants, context, &io);
  else pipeline.vertex()(attributes, constants, &io);
  ClipVertex out;
  out.position = io.position;
  for (uint32_t i = 0; i < kMaxVaryings; ++i) out.varying[i] = io.varying[i];
  out.layer = io.layer;         // (a vertex shader's SV_RenderTargetArrayIndex and SV_ViewportArrayIndex: kFeature2LayerFromAnyStage)
  out.viewport = io.viewport;
  return out;
}

// Runs a geometry shader on one primitive and draws what it emits: the vertices are a strip (a triangle strip turns every second triangle over, as Direct3D does) or points.
// Writes the primitives of the streams the pipeline declares stream output for: each stream's vertices are made into points, lines or triangles as for drawing, and every vertex of a primitive is a
// record in each buffer its entries name (the entries of a buffer in order, one after the other from the start of the record; the rest of the record is as it was). A primitive that does not
// fit in the buffers it goes to is not written, nor is any after it of its stream.
// Writes one primitive of `count` vertices to the stream output buffers `used_buffers` (bit n: buffer n) takes from `stream`, and counts it for a statistics query. A primitive that does not fit
// in every one of them is dropped and closes them. False if it was not written.
bool store_stream_output_primitive(const DrawState& s, uint32_t stream, uint32_t used_buffers, const ClipVertex* vertex, uint32_t count) {
  const GraphicsPipelineDesc& pd = s.pipeline->desc();
  if (s.counters) ++s.counters->so[2 * stream + 1];
  if (!used_buffers) return false;
  bool overflow = false;
  for (uint32_t b = 0; b < kMaxStreamOutputBuffers; ++b)
    if ((used_buffers >> b & 1) && (s.stream_output[b].closed || s.stream_output[b].written + uint64_t{count} * pd.stream_output_stride[b] > s.stream_output[b].view.size())) overflow = true;
  if (overflow) {
    for (uint32_t b = 0; b < kMaxStreamOutputBuffers; ++b)
      if (used_buffers >> b & 1) s.stream_output[b].closed = true;
    return false;
  }
  if (s.counters) ++s.counters->so[2 * stream];
  for (uint32_t k = 0; k < count; ++k) {
    for (uint32_t b = 0; b < kMaxStreamOutputBuffers; ++b) {
      if (!(used_buffers >> b & 1)) continue;
      ByteView record;
      if (!s.stream_output[b].view.try_subview(s.stream_output[b].written + uint64_t{k} * pd.stream_output_stride[b], pd.stream_output_stride[b], &record)) continue;
      ByteWriter out(record);
      for (uint32_t e = 0; e < pd.stream_output_count; ++e) {
        const StreamOutputEntry& entry = pd.stream_output[e];
        if (entry.stream != stream || entry.buffer != b) continue;
        // (the position goes out as the shader wrote it: the devices' +Y down is turned over)
        Vec4 value = entry.output == 0 ? Vec4{vertex[k].position.x, -vertex[k].position.y, vertex[k].position.z, vertex[k].position.w} : vertex[k].varying[(entry.output - 1) % kMaxVaryings];
        const float components[4] = {value.x, value.y, value.z, value.w};
        for (uint32_t c = 0; c < entry.component_count; ++c) out.le32(__builtin_bit_cast(uint32_t, components[(entry.start_component + c) & 3]));
      }
    }
  }
  for (uint32_t b = 0; b < kMaxStreamOutputBuffers; ++b)
    if (used_buffers >> b & 1) s.stream_output[b].written += uint64_t{count} * pd.stream_output_stride[b];
  return true;
}

uint32_t stream_output_buffers_of(const GraphicsPipelineDesc& pd, uint32_t stream) {
  uint32_t used = 0;
  for (uint32_t e = 0; e < pd.stream_output_count; ++e)
    if (pd.stream_output[e].stream == stream) used |= 1u << pd.stream_output[e].buffer;
  return used;
}

// The primitives a geometry shader made, to the streams they belong to.
void capture_stream_output(const DrawState& s, const ShaderModuleDesc& md, const GeometryOutputData& output) {
  const GraphicsPipelineDesc& pd = s.pipeline->desc();
  if (!pd.stream_output_count || !s.stream_output) return;
  const GeometryTopology topology = md.geometry_streams > 1 ? GeometryTopology::PointList : md.geometry_output;
  const uint32_t need = topology == GeometryTopology::PointList ? 1 : topology == GeometryTopology::LineStrip ? 2 : 3;
  for (uint32_t stream = 0; stream < md.geometry_streams && stream < kMaxGeometryStreams; ++stream) {
    const uint32_t used_buffers = stream_output_buffers_of(pd, stream);   // (bit n: buffer n gets this stream)
    if (!used_buffers && !s.counters) continue;
    uint32_t strip[kMaxGeometryVertices];
    uint32_t strip_length = 0;   // the vertices of this stream's strip so far (all of them for a list of points)
    for (uint32_t i = 0; i < output.count; ++i) {
      if (output.stream[i] != stream) continue;
      if (output.restart[i]) strip_length = 0;
      strip[strip_length++] = i;
      uint32_t primitive[3];
      uint32_t count = 0;
      if (topology == GeometryTopology::PointList) {
        primitive[count++] = i;
        strip_length = 0;
      } else if (strip_length >= need) {
        // (the last `need` vertices of the strip; every second triangle's first two swapped, as the rasterizer winds them)
        const uint32_t j = strip_length - 1;
        if (topology == GeometryTopology::LineStrip) {
          primitive[0] = strip[j - 1], primitive[1] = strip[j];
        } else if ((strip_length - 3) & 1) {
          primitive[0] = strip[j - 1], primitive[1] = strip[j - 2], primitive[2] = strip[j];
        } else {
          primitive[0] = strip[j - 2], primitive[1] = strip[j - 1], primitive[2] = strip[j];
        }
        count = need;
      } else {
        continue;
      }
      ClipVertex vertices[3];
      for (uint32_t k = 0; k < count; ++k) {
        vertices[k].position = output.vertex[primitive[k]].position;
        for (uint32_t n = 0; n < kMaxVaryings; ++n) vertices[k].varying[n] = output.vertex[primitive[k]].varying[n];
      }
      store_stream_output_primitive(s, stream, used_buffers, vertices, count);
    }
  }
}

// The primitives of a vertex or domain shader (no geometry shader): one stream, stream 0.
void capture_stream_output_primitive(const DrawState& s, const ClipVertex* vertex, uint32_t count) {
  const GraphicsPipelineDesc& pd = s.pipeline->desc();
  if (!pd.stream_output_count || !s.stream_output) return;
  store_stream_output_primitive(s, 0, stream_output_buffers_of(pd, 0), vertex, count);
}

// The primitives a geometry shader's output makes, on all its streams (several streams are lists of points; one is strips or points).
uint64_t geometry_primitives(const ShaderModuleDesc& md, const GeometryOutputData& output) {
  const GeometryTopology topology = md.geometry_streams > 1 ? GeometryTopology::PointList : md.geometry_output;
  if (topology == GeometryTopology::PointList) return output.count;
  const uint32_t need = topology == GeometryTopology::LineStrip ? 2 : 3;
  uint64_t primitives = 0;
  uint32_t strip = 0;
  for (uint32_t i = 0; i < output.count; ++i) {
    if (output.restart[i]) strip = 0;
    if (++strip >= need) ++primitives;
  }
  return primitives;
}

void run_geometry_instance(const DrawState& s, const ShaderModuleDesc& md, const GeometryInputData& input) {
  GeometryOutputData output;
  md.geometry_function(input, s.constants, &output);
  if (output.count > md.geometry_max_vertices) output.count = md.geometry_max_vertices;
  s.count(PipelineStatistic::GsInvocations);
  s.count(PipelineStatistic::GsPrimitives, geometry_primitives(md, output));
  capture_stream_output(s, md, output);
  // (with stream output only the stream that is rasterized is drawn, if any)
  const int32_t drawn_stream = s.pipeline->desc().stream_output_count ? s.pipeline->desc().rasterized_stream : 0;
  if (drawn_stream < 0) return;
  auto clip = [&](uint32_t i) {
    ClipVertex c;
    c.position = output.vertex[i].position;
    for (uint32_t k = 0; k < kMaxVaryings; ++k) c.varying[k] = output.vertex[i].varying[k];
    c.layer = output.vertex[i].layer;
    c.viewport = output.vertex[i].viewport;
    return c;
  };
  uint32_t in_strip = 0;   // vertices of the strip so far
  for (uint32_t i = 0; i < output.count; ++i) {
    if (md.geometry_streams > 1 && output.stream[i] != static_cast<uint32_t>(drawn_stream)) continue;   // (several streams are lists of points: the one that is drawn)
    if (output.restart[i]) in_strip = 0;
    ++in_strip;
    if (md.geometry_output == GeometryTopology::PointList) {
      draw_point(s, clip(i));
    } else if (md.geometry_output == GeometryTopology::LineStrip) {
      if (in_strip >= 2) draw_line(s, clip(i - 1), clip(i));
    } else if (in_strip >= 3) {
      const bool odd = (in_strip - 3) & 1;
      if (odd) {
        ClipVertex first = clip(i - 1);
        first.layer = output.vertex[i - 2].layer;   // (the triangle's leading vertex in the strip's order names its layer and viewport)
        first.viewport = output.vertex[i - 2].viewport;
        draw_triangle(s, first, clip(i - 2), clip(i));
      }
      else draw_triangle(s, clip(i - 2), clip(i - 1), clip(i));
    }
  }
}

void run_geometry_shader(const DrawState& s, const SoftwareShaderModule& module, const ClipVertex* v, uint32_t count) {
  const ShaderModuleDesc& md = module.desc();
  GeometryInputData input;
  input.storage = s.storage;
  input.images = s.images;
  for (uint32_t k = 0; k < count && k < kMaxGeometryInputVertices; ++k) {
    input.position[k] = v[k].position;
    for (uint32_t i = 0; i < kMaxVaryings; ++i) input.varying[k][i] = v[k].varying[i];
  }
  input.primitive_id = s.primitive_id;
  for (uint32_t instance = 0; instance < md.geometry_instances; ++instance) {
    input.instance_id = instance;
    run_geometry_instance(s, md, input);
  }
}

// Tessellates one patch: the hull shader makes the output control points and the factors, the tessellator the domain points and the primitives joining them, and the domain shader turns each
// point into a vertex (a triangle's w is 1 - u - v; a quad's and an isoline's shader has no w). Triangles are drawn in the order the tessellator made them, the way a Direct3D pipeline
// hands them to the rasterizer; a patch the tessellator culls draws nothing.
void run_tessellation(allocators::IAllocator& allocator, const DrawState& s, Tessellator& tessellator, const SoftwareShaderModule& hull, const SoftwareShaderModule& domain, const ClipVertex* v, uint32_t count) {
  const ShaderModuleDesc& hd = hull.desc();
  HullInputData input;
  input.storage = s.storage;
  input.images = s.images;
  input.control_points = count;
  input.primitive_id = s.primitive_id;
  for (uint32_t k = 0; k < count && k < kMaxPatchControlPoints; ++k) {
    input.position[k] = v[k].position;
    for (uint32_t i = 0; i < kMaxVaryings; ++i) input.varying[k][i] = v[k].varying[i];
  }
  HullOutputData patch;
  s.count(PipelineStatistic::HsInvocations);
  hd.hull_function(input, s.constants, &patch);
  patch.storage = s.storage;
  patch.images = s.images;
  if (!tessellator.tessellate(hd.tess_domain, hd.tess_partitioning, hd.tess_output, patch.outer, patch.inner)) return;
  const DomainShaderFn ds = domain.desc().domain_function;
  const bool triangles = hd.tess_domain == TessDomain::Triangle;
  const ArrayView<const Tessellator::Point> points = tessellator.points();
  const ArrayView<const uint16_t> indices = tessellator.indices();
  // A domain shader that writes buffers runs once for each point the tessellator makes, as the hardware's does (not again for each triangle that uses the point): the vertices are kept.
  const bool once = s.pipeline->desc().tessellation_stores;
  ArrayView<ClipVertex> kept = once ? allocate_view<ClipVertex>(allocator, points.size()) : ArrayView<ClipVertex>();
  ArrayView<uint8_t> have = once ? allocate_view<uint8_t>(allocator, points.size()) : ArrayView<uint8_t>();
  struct Release {
    allocators::IAllocator& allocator;
    ArrayView<ClipVertex>& vertices;
    ArrayView<uint8_t>& flags;
    ~Release() {
      release_view(allocator, vertices);
      release_view(allocator, flags);
    }
  } release{allocator, kept, have};
  if (once && (kept.empty() || have.empty())) return;
  if (once) have.fill(0);
  auto vertex = [&](uint32_t index) {
    if (once && have[index]) return kept[index];
    const Tessellator::Point p = points[index];
    ShaderIo io;
    s.count(PipelineStatistic::DsInvocations);
    ds(patch, p.u, p.v, triangles ? 1.0f - p.u - p.v : 0.0f, s.constants, &io);
    ClipVertex c;
    c.position = io.position;
    for (uint32_t k = 0; k < kMaxVaryings; ++k) c.varying[k] = io.varying[k];
    c.layer = io.layer;
    c.viewport = io.viewport;
    if (once) kept[index] = c, have[index] = 1;
    return c;
  };
  const bool rasterize = !s.pipeline->desc().stream_output_count || s.pipeline->desc().rasterized_stream == 0;
  if (hd.tess_output == TessOutput::Point) {
    for (const uint16_t i : indices) {
      const ClipVertex a = vertex(i);
      capture_stream_output_primitive(s, &a, 1);
      if (rasterize) draw_point(s, a);
    }
  } else if (hd.tess_output == TessOutput::Line) {
    for (uint64_t i = 0; i + 2 <= indices.size(); i += 2) {
      const ClipVertex a[2] = {vertex(indices[i]), vertex(indices[i + 1])};
      capture_stream_output_primitive(s, a, 2);
      if (rasterize) draw_line(s, a[0], a[1]);
    }
  } else {
    for (uint64_t i = 0; i + 3 <= indices.size(); i += 3) {
      const ClipVertex a[3] = {vertex(indices[i]), vertex(indices[i + 1]), vertex(indices[i + 2])};
      capture_stream_output_primitive(s, a, 3);
      if (rasterize) draw_triangle(s, a[0], a[1], a[2]);
    }
  }
}

}  // namespace

// --- Device ------------------------------------------------------------------

IBuffer* SoftwareDevice::create_buffer(uint64_t size, uint32_t) {
  SoftwareBuffer* b = make<SoftwareBuffer>(allocator_, allocator_, size);
  if (b && !b->valid()) {
    destroy(allocator_, b);
    return nullptr;
  }
  return b;
}
void SoftwareDevice::destroy_buffer(IBuffer* buffer) { destroy(allocator_, static_cast<SoftwareBuffer*>(buffer)); }

IImage* SoftwareDevice::create_image(const ImageDesc& desc) {
  if (!desc.extent.width || !desc.extent.height || desc.format == Format::Undefined || desc.extent.width > max_image_dimension() || desc.extent.height > max_image_dimension()) return nullptr;
  // (a format numbered from 64: what format_caps says; block-compressed images are decoded by the memory manager before they get here)
  if (static_cast<uint32_t>(desc.format) >= static_cast<uint32_t>(Format::R8Unorm)) {
    const uint32_t caps = format_caps(desc.format);
    if (is_compressed_format(desc.format) || !(caps & kFormatSampled) || ((desc.usage & kImageColorAttachment) && !(caps & kFormatRenderTarget)) ||
        ((desc.usage & kImageStorage) && !(caps & kFormatStorage)) || (desc.mip_levels > 1 && !(caps & kFormatMipmaps)) || desc.samples != 1)
      return nullptr;
  }
  // (any image of one sample may have levels, as many as the extent has: kFeature2MipmappedLayers)
  if (desc.mip_levels == 0 || desc.mip_levels > mip_level_limit(desc.extent) || desc.mip_levels > kMaxMipLevels) return nullptr;
  if (desc.samples != 1) {
    // (a multisampled image is an 8-bit colour or a depth attachment, alone, of one level)
    const bool depth = desc.format == Format::D32Float;
    if ((desc.samples != 2 && desc.samples != 4 && desc.samples != 8) || desc.mip_levels != 1 || desc.kind != TextureKind::Texture2D || is_wide_format(desc.format) ||
        (depth ? desc.usage != kImageDepthAttachment : (desc.usage & ~kImageSampled) != kImageColorAttachment))
      return nullptr;
  }
  // (a storage image: a sampled colour image of one sample that is not sRGB or block-compressed; a cube's or a cube array's faces are an array of them to its shaders)
  if ((desc.usage & kImageStorage) && (!(desc.usage & kImageSampled) || desc.samples != 1 || desc.format == Format::D32Float || is_srgb_format(desc.format) || is_compressed_format(desc.format)))
    return nullptr;
  if (desc.kind != TextureKind::Texture2D) {
    // (layered images: sampled, and also drawn into (colour or depth) or read and written as storage images; a cube is six square layers, a cube array six times its cubes)
    const uint32_t layers = image_layer_count(desc.kind, desc.layers);
    const bool depth = desc.format == Format::D32Float;
    const bool cube = desc.kind == TextureKind::TextureCube || desc.kind == TextureKind::TextureCubeArray;
    if (!(desc.usage & (depth ? kImageDepthAttachment : kImageSampled)) || (desc.usage & ~(depth ? kImageDepthAttachment | kImageSampled : kImageSampled | kImageColorAttachment | kImageStorage)) ||
        layers == 0 || layers > kMaxTextureLayers || (depth && desc.kind == TextureKind::Texture3D) || desc.samples != 1 || desc.kind == TextureKind::Texture2DMS ||
        (cube && desc.extent.width != desc.extent.height) || (desc.kind == TextureKind::TextureCubeArray && layers % 6 != 0))
      return nullptr;
  } else if (desc.layers != 1) {
    return nullptr;
  }
  SoftwareImage* image = make<SoftwareImage>(allocator_, allocator_, desc);
  if (image && !image->valid()) {
    destroy(allocator_, image);
    return nullptr;
  }
  return image;
}
void SoftwareDevice::destroy_image(IImage* image) { destroy(allocator_, static_cast<SoftwareImage*>(image)); }

IShaderModule* SoftwareDevice::create_shader_module(const ShaderModuleDesc& desc) {
  if (desc.stage == ShaderStage::Hull || desc.stage == ShaderStage::Domain) {
    // (any domain, with up to the most control points)
    if (desc.stage == ShaderStage::Hull ? !desc.hull_function : !desc.domain_function) return nullptr;
    if (desc.tess_input_points == 0 || desc.tess_input_points > kMaxPatchControlPoints || desc.tess_output_points == 0 || desc.tess_output_points > kMaxPatchControlPoints) return nullptr;
    return make<SoftwareShaderModule>(allocator_, desc);
  }
  if (desc.stage == ShaderStage::Geometry) {
    if (!desc.geometry_function || desc.geometry_max_vertices == 0 || desc.geometry_max_vertices > kMaxGeometryVertices || desc.geometry_streams == 0 || desc.geometry_streams > kMaxGeometryStreams) return nullptr;
    return make<SoftwareShaderModule>(allocator_, desc);
  }
  if (desc.stage == ShaderStage::Compute) {
    if (!desc.compute_function && !desc.compute_group_function) return nullptr;
    uint64_t threads = 1;
    for (uint32_t axis : desc.thread_group) {
      if (axis == 0 || axis > kMaxThreadsPerGroup) return nullptr;
      threads *= axis;
    }
    if (threads > kMaxThreadsPerGroup) return nullptr;
    return make<SoftwareShaderModule>(allocator_, desc);
  }
  if (desc.stage == ShaderStage::Vertex ? !(desc.vertex_function || desc.vertex_function_ex) : !(desc.fragment_function_ex || (!desc.fragment_function) != (!desc.textured_fragment_function))) return nullptr;   // (a fragment shader is one or the other)
  return make<SoftwareShaderModule>(allocator_, desc);
}
void SoftwareDevice::destroy_shader_module(IShaderModule* module) {
  destroy(allocator_, static_cast<SoftwareShaderModule*>(module));
}

IPipeline* SoftwareDevice::create_compute_pipeline(const IShaderModule* shader) {
  if (!shader || shader->stage() != ShaderStage::Compute) return nullptr;
  const SoftwareShaderModule* module = static_cast<const SoftwareShaderModule*>(shader);
  return make<SoftwareComputePipeline>(allocator_, module->desc().compute_function, module->desc().compute_group_function, module->desc().thread_group);
}

IPipeline* SoftwareDevice::create_graphics_pipeline(const GraphicsPipelineDesc& desc) {
  if (!desc.vertex_shader || !desc.fragment_shader || desc.varying_count > kMaxVaryings ||
      desc.attribute_count > kMaxVertexAttributes)
    return nullptr;
  const auto* vs = static_cast<const SoftwareShaderModule*>(desc.vertex_shader);
  const auto* fs = static_cast<const SoftwareShaderModule*>(desc.fragment_shader);
  if (vs->stage() != ShaderStage::Vertex || fs->stage() != ShaderStage::Fragment) return nullptr;
  if (desc.geometry_shader && desc.geometry_shader->stage() != ShaderStage::Geometry) return nullptr;
  if (desc.stream_output_count) {
    // (stream output takes the outputs of the geometry shader, or the domain shader, or the vertex shader if there are neither; entries that name its streams, outputs and buffers, and buffers
    // that have only one stream)
    if (desc.stream_output_count > kMaxStreamOutputEntries || desc.rasterized_stream < -1 || desc.rasterized_stream >= static_cast<int32_t>(kMaxGeometryStreams)) return nullptr;
    const uint32_t streams = desc.geometry_shader ? static_cast<const SoftwareShaderModule*>(desc.geometry_shader)->desc().geometry_streams : 1;
    if (!desc.geometry_shader && desc.rasterized_stream > 0) return nullptr;
    int32_t stream_of_buffer[kMaxStreamOutputBuffers] = {-1, -1, -1, -1};
    uint32_t bytes[kMaxStreamOutputBuffers] = {};
    for (uint32_t e = 0; e < desc.stream_output_count; ++e) {
      const StreamOutputEntry& entry = desc.stream_output[e];
      if (entry.stream >= streams || entry.output > kMaxVaryings || entry.component_count == 0 || entry.component_count > 4 || entry.start_component > 3 || entry.start_component + entry.component_count > 4 || entry.buffer >= kMaxStreamOutputBuffers) return nullptr;
      if (stream_of_buffer[entry.buffer] >= 0 && stream_of_buffer[entry.buffer] != entry.stream) return nullptr;
      stream_of_buffer[entry.buffer] = entry.stream;
      bytes[entry.buffer] += 4u * entry.component_count;
    }
    for (uint32_t b = 0; b < kMaxStreamOutputBuffers; ++b)
      if (stream_of_buffer[b] >= 0 && (desc.stream_output_stride[b] < bytes[b] || desc.stream_output_stride[b] % 4 || desc.stream_output_stride[b] > 2048)) return nullptr;
  }
  if (desc.hull_shader || desc.domain_shader) {
    if (!desc.hull_shader || !desc.domain_shader || desc.hull_shader->stage() != ShaderStage::Hull || desc.domain_shader->stage() != ShaderStage::Domain) return nullptr;
    const ShaderModuleDesc& h = static_cast<const SoftwareShaderModule*>(desc.hull_shader)->desc();
    const ShaderModuleDesc& d = static_cast<const SoftwareShaderModule*>(desc.domain_shader)->desc();
    if (h.tess_output_points != d.tess_input_points || h.tess_domain != d.tess_domain) return nullptr;   // (the domain shader takes the hull shader's control points and domain)
  }
  // A shader that samples textures needs the pipeline to say how many, and the other way round.
  // (one with system values reads whatever it likes)
  const bool textured = fs->desc().textured_fragment_function != nullptr;
  // (textures without a fragment shader that samples them: those of a vertex shader that reads them, through its context)
  const bool vertex_reads = vs->desc().vertex_function_ex != nullptr;
  if (desc.texture_count > kMaxTextureUnits || (!fs->desc().fragment_function_ex && (textured ? desc.texture_count == 0 : desc.texture_count != 0 && !vertex_reads))) return nullptr;
  return make<SoftwarePipeline>(allocator_, desc, vs->desc().vertex_function, fs->desc().fragment_function, fs->desc().textured_fragment_function, vs->desc().vertex_function_ex,
                                fs->desc().fragment_function_ex);
}
void SoftwareDevice::destroy_pipeline(IPipeline* pipeline) {
  destroy(allocator_, static_cast<SoftwarePipeline*>(pipeline));
}

ICommandBuffer* SoftwareDevice::create_command_buffer() { return make<CommandRecorder>(allocator_, allocator_); }
void SoftwareDevice::destroy_command_buffer(ICommandBuffer* commands) {
  destroy(allocator_, static_cast<CommandRecorder*>(commands));
}

bool SoftwareDevice::submit(ICommandBuffer* commands) {
  if (!commands) return false;
  CommandRecorder& recorder = *static_cast<CommandRecorder*>(commands);
  DrawState s;
  const bool interrupted = clock_interrupted_;   // (what a disjoint query in these commands says, and then it is said)
  bool disjoint_asked = false;
  // (how far each stream output buffer got, for the next bind that appends)
  auto remember_filled = [](const DrawState::StreamOutputTarget& target) {
    if (target.buffer) target.buffer->set_stream_output_filled(target.base + target.written);
  };
  const SoftwareComputePipeline* compute = nullptr;
  ByteView storage[kMaxStorageBuffers];
  s.storage = storage;
  StorageImageSet images;
  s.images = &images;
  DrawState::StreamOutputTarget stream_output[kMaxStreamOutputBuffers];
  // The queries: what the commands so far did, and each open query's slot, kind and what the counts were when it began.
  QueryCounters counters;
  s.counters = &counters;
  struct OpenQuery {
    uint32_t slot;
    QueryKind kind;
    uint32_t stream;
    QueryCounters at_begin;
  };
  List<OpenQuery> open_queries(allocator_);
  IBuffer* vertex_buffers[kMaxVertexBuffers] = {};
  uint64_t vertex_offsets[kMaxVertexBuffers] = {};
  IBuffer* index_buffer = nullptr;
  uint64_t index_offset = 0;
  IndexType index_type = IndexType::Uint16;
  PrimitiveTopology topology = PrimitiveTopology::TriangleList;
  uint32_t patch_points = 0;
  bool in_pass = false;
  s.constants = s.constants_storage;
  s.stream_output = stream_output;
  const ConstByteView pool = recorder.constants();

  for (uint64_t i = 0; i < recorder.commands().count(); ++i) {
    const RecordedCommand& cmd = recorder.commands()[i];
    switch (cmd.type) {
      case RecordedCommand::Type::BeginRenderPass: {
        if (in_pass || !cmd.pass.color) return false;
        s.color = static_cast<SoftwareImage*>(cmd.pass.color);
        s.depth = static_cast<SoftwareImage*>(cmd.pass.depth);
        if (!(s.color->usage() & kImageColorAttachment) || s.color->format() == Format::D32Float) return false;
        if (s.depth && s.depth->format() != Format::D32Float) return false;
        s.sample_count = s.color->samples();
        if (s.depth && s.depth->samples() != s.sample_count) return false;
        // (every target's level is of one size: the pass's)
        for (uint32_t n = 0; n <= kMaxColorTargets; ++n) s.level[n] = cmd.pass.level[n];
        if (s.level[0] >= s.color->mip_levels() || (s.depth && s.level[kMaxColorTargets] >= s.depth->mip_levels())) return false;
        const Extent2D e = s.color->level_extent(s.level[0]);
        s.extent = e;
        if (s.depth && (s.depth->level_extent(s.level[kMaxColorTargets]).width != e.width || s.depth->level_extent(s.level[kMaxColorTargets]).height != e.height)) return false;
        for (SoftwareImage*& extra : s.extra_colors) extra = nullptr;
        for (uint32_t n = 0; n + 1 < kMaxColorTargets && cmd.pass.extra_colors[n]; ++n) {
          SoftwareImage* image = static_cast<SoftwareImage*>(cmd.pass.extra_colors[n]);
          if (!(image->usage() & kImageColorAttachment) || image->format() == Format::D32Float || image == s.color || image == s.depth) return false;
          if (s.level[n + 1] >= image->mip_levels() || image->level_extent(s.level[n + 1]).width != e.width || image->level_extent(s.level[n + 1]).height != e.height || image->samples() != s.sample_count) return false;
          for (uint32_t k = 0; k < n; ++k)
            if (s.extra_colors[k] == image) return false;
          s.extra_colors[n] = image;
        }
        // (the layers of each target: within its level; a 2D image has layer 0 alone)
        s.layer = 0;
        for (uint32_t n = 0; n <= kMaxColorTargets; ++n) {
          const SoftwareImage* image = n == kMaxColorTargets ? s.depth : n == 0 ? s.color : s.extra_colors[n - 1];
          s.first_layer[n] = cmd.pass.first_layer[n];
          s.layer_count[n] = cmd.pass.layer_count[n];
          if (image && (s.layer_count[n] == 0 || s.first_layer[n] >= image->level_layers(s.level[n]) || s.layer_count[n] > image->level_layers(s.level[n]) - s.first_layer[n])) return false;
        }
        if (cmd.pass.clear_color) {
          for (uint32_t n = 0; n < kMaxColorTargets; ++n) {
            const SoftwareImage* image = n == 0 ? s.color : s.extra_colors[n - 1];
            if (!image) break;
            // (the pass's layers of the target's level; every sample of every pixel)
            for (uint32_t l = 0; l < s.layer_count[n]; ++l) {
              const Surface surface = image->surface(s.level[n], s.first_layer[n] + l);
              if (!surface.floats.empty()) {
                for (Vec4& c : surface.floats) c = image->store_float(cmd.pass.clear_color_value);
              } else {
                surface.pixels.fill(surface.encode(is_srgb_format(surface.format) ? pack_srgb(cmd.pass.clear_color_value) : pack_color(cmd.pass.clear_color_value)));
              }
            }
          }
        }
        for (uint32_t l = 0; s.depth && l < s.layer_count[kMaxColorTargets]; ++l) {
          if (cmd.pass.clear_depth) s.depth->depth_layer(s.first_layer[kMaxColorTargets] + l, s.level[kMaxColorTargets]).fill(cmd.pass.clear_depth_value);
          if (cmd.pass.clear_stencil) s.depth->stencil_layer(s.first_layer[kMaxColorTargets] + l, s.level[kMaxColorTargets]).fill(cmd.pass.clear_stencil_value);
        }
        s.viewport = s.viewports[0] = {0, 0, static_cast<float>(e.width), static_cast<float>(e.height), 0, 1};
        s.scissor = s.scissors[0] = {0, 0, e.width, e.height};
        s.viewport_count = s.scissor_count = 1;
        s.pipeline = nullptr;
        in_pass = true;
        break;
      }
      case RecordedCommand::Type::EndRenderPass:
        in_pass = false;
        break;
      case RecordedCommand::Type::Resolve: {
        // The samples of each pixel averaged (to the nearest, per channel) into the single-sample image.
        SoftwareImage* to = static_cast<SoftwareImage*>(cmd.image);
        const SoftwareImage* from = static_cast<const SoftwareImage*>(cmd.image2);
        if (in_pass || !to || !from || to->samples() != 1 || from->samples() < 2 || to->format() != from->format() || is_wide_format(to->format()) ||
            to->extent().width != from->extent().width || to->extent().height != from->extent().height || cmd.count > static_cast<uint32_t>(ResolveMode::Max))
          return false;
        const ResolveMode mode = static_cast<ResolveMode>(cmd.count);
        const uint32_t n = from->samples();
        if (to->format() == Format::D32Float) {
          // A depth image: the depth of sample 0, or the least, the greatest or the mean of the samples; the stencil plane takes sample 0's.
          if (to->depth().empty() || from->depth().size() < to->depth().size() * n) return false;
          const uint64_t pixels = uint64_t{to->extent().width} * to->extent().height;
          for (uint64_t i = 0; i < pixels; ++i) {
            const float* samples = &from->depth()[i * n];
            float value = samples[0];
            for (uint32_t k = 1; k < n; ++k) {
              if (mode == ResolveMode::Min) value = samples[k] < value ? samples[k] : value;
              else if (mode == ResolveMode::Max) value = samples[k] > value ? samples[k] : value;
              else if (mode == ResolveMode::Average) value += samples[k];
            }
            to->depth()[i] = mode == ResolveMode::Average ? value / static_cast<float>(n) : value;
            if (!to->stencil().empty() && !from->stencil().empty()) to->stencil()[i] = from->stencil()[i * n];
          }
          break;
        }
        const Surface out = to->surface(), in = from->surface();
        if (mode == ResolveMode::Sample0) {
          for (uint32_t y = 0; y < out.height; ++y)
            for (uint32_t x = 0; x < out.width; ++x) out.row(y)[x] = in.row(y)[uint64_t{x} * n];
          break;
        }
        if (mode == ResolveMode::Min || mode == ResolveMode::Max) {
          // (the least or greatest of each channel's codes: the order of the codes is the order of the values, sRGB ones too)
          for (uint32_t y = 0; y < out.height; ++y)
            for (uint32_t x = 0; x < out.width; ++x) {
              uint32_t best = in.decode(in.row(y)[uint64_t{x} * n]);
              for (uint32_t k = 1; k < n; ++k) {
                const uint32_t argb = in.decode(in.row(y)[uint64_t{x} * n + k]);
                uint32_t merged = 0;
                for (uint32_t c = 0; c < 4; ++c) {
                  const uint32_t a = (best >> (8 * c)) & 0xFF, b = (argb >> (8 * c)) & 0xFF;
                  merged |= (mode == ResolveMode::Min ? (a < b ? a : b) : (a > b ? a : b)) << (8 * c);
                }
                best = merged;
              }
              out.row(y)[x] = out.encode(best);
            }
          break;
        }
        for (uint32_t y = 0; y < out.height; ++y)
          for (uint32_t x = 0; x < out.width; ++x) {
            if (is_srgb_format(in.format)) {
              // (an sRGB image is averaged as the linear values its codes mean, and the average encoded again)
              const float* table = srgb_to_linear_table();
              Vec4 mean = {0, 0, 0, 0};
              uint32_t alpha = 0;
              for (uint32_t k = 0; k < n; ++k) {
                const uint32_t argb = in.decode(in.row(y)[uint64_t{x} * n + k]);
                mean.x += table[(argb >> 16) & 0xFF], mean.y += table[(argb >> 8) & 0xFF], mean.z += table[argb & 0xFF];
                alpha += argb >> 24;
              }
              const float inv = 1.0f / static_cast<float>(n);
              out.row(y)[x] = out.encode(pack_srgb({mean.x * inv, mean.y * inv, mean.z * inv, static_cast<float>(alpha) * inv * (1.0f / 255.0f)}));
              continue;
            }
            uint32_t sum[4] = {};
            for (uint32_t k = 0; k < n; ++k) {
              const uint32_t argb = in.decode(in.row(y)[uint64_t{x} * n + k]);
              for (uint32_t c = 0; c < 4; ++c) sum[c] += (argb >> (8 * c)) & 0xFF;
            }
            uint32_t argb = 0;
            for (uint32_t c = 0; c < 4; ++c) argb |= ((sum[c] + n / 2) / n) << (8 * c);
            out.row(y)[x] = out.encode(argb);
          }
        break;
      }
      case RecordedCommand::Type::BindComputePipeline:
        if (in_pass) return false;
        compute = static_cast<const SoftwareComputePipeline*>(cmd.pipeline);
        break;
      case RecordedCommand::Type::BindStreamOutput: {
        if (cmd.unit >= kMaxStreamOutputBuffers) return false;
        remember_filled(stream_output[cmd.unit]);
        stream_output[cmd.unit] = {};
        if (cmd.buffer) {
          ByteView whole = cmd.buffer->map(), part;
          const uint64_t start = cmd.offset == kStreamOutputAppend ? cmd.buffer->stream_output_filled() : cmd.offset;
          if (start > whole.size() || !whole.try_subview(start, whole.size() - start, &part)) return false;
          stream_output[cmd.unit].view = part;
          stream_output[cmd.unit].buffer = cmd.buffer;
          stream_output[cmd.unit].base = start;
          cmd.buffer->set_stream_output_filled(start);
        }
        break;
      }
      case RecordedCommand::Type::BindStorageImage: {
        SoftwareImage* image = static_cast<SoftwareImage*>(cmd.image);
        const StorageImageView& view = cmd.storage_view;
        if (cmd.unit >= kMaxStorageImages) return false;
        if (image && (!(image->usage() & kImageStorage) || view.level >= image->mip_levels() || view.layer_count == 0 || view.first_layer >= image->level_layers(view.level) ||
                      view.layer_count > image->level_layers(view.level) - view.first_layer || !storage_view_compatible(image->format(), view.format)))
          return false;
        images.bind(cmd.unit, image, view);
        break;
      }
      case RecordedCommand::Type::BindStorageTexelBuffer: {
        // (a typed view of a buffer: a format the slot takes - the u# ones a format that can be stored -, and elements that lie in the buffer from an offset of whole elements)
        const TexelBufferView& view = cmd.texel_view;
        if (cmd.unit >= kMaxImageSlots) return false;
        if (!cmd.buffer) {
          images.bind_texel_buffer(cmd.unit, ByteView(), TexelFormat::Undefined);
          break;
        }
        const uint32_t bytes = texel_bytes(view.format);
        if (!texel_format_valid(view.format) || (cmd.unit < kMaxStorageImages && !texel_format_storable(view.format)) || view.elements == 0 || view.offset % bytes) return false;
        ByteView part;
        if (!cmd.buffer->map().try_subview(view.offset, uint64_t{view.elements} * bytes, &part)) return false;
        images.bind_texel_buffer(cmd.unit, part, view.format);
        break;
      }
      case RecordedCommand::Type::BindStorageBuffer: {
        if (cmd.unit >= kMaxStorageBuffers || !cmd.buffer) return false;   // (the bindings may change between draws)
        ByteView whole = cmd.buffer->map(), part;
        if (!whole.try_subview(cmd.offset, cmd.count, &part)) return false;
        storage[cmd.unit] = part;
        break;
      }
      case RecordedCommand::Type::Dispatch: {
        if (in_pass || !compute) return false;
        const uint32_t gx = cmd.count, gy = cmd.first, gz = cmd.instance_count;
        if (gx > kMaxDispatchGroups || gy > kMaxDispatchGroups || gz > kMaxDispatchGroups) return false;
        const uint64_t threads_per_group = uint64_t{compute->group(0)} * compute->group(1) * compute->group(2);
        if (uint64_t{gx} * gy * gz * threads_per_group > kMaxDispatchThreads) return false;
        s.count(PipelineStatistic::CsInvocations, uint64_t{gx} * gy * gz * threads_per_group);
        ComputeContext context;
        for (uint32_t k = 0; k < kMaxStorageBuffers; ++k) context.buffers[k] = storage[k];
        context.textures = &s.textures;   // (bound outside the passes as well)
        context.images = &images;
        for (uint32_t z = 0; z < gz; ++z)
          for (uint32_t y = 0; y < gy; ++y)
            for (uint32_t x = 0; x < gx; ++x) {
              context.group_id[0] = x, context.group_id[1] = y, context.group_id[2] = z;
              if (compute->group_function()) {
                compute->group_function()(context, s.constants);   // (the shader runs the threads of the group itself)
                continue;
              }
              uint32_t index = 0;
              for (uint32_t tz = 0; tz < compute->group(2); ++tz)
                for (uint32_t ty = 0; ty < compute->group(1); ++ty)
                  for (uint32_t tx = 0; tx < compute->group(0); ++tx, ++index) {
                    context.thread_id[0] = tx, context.thread_id[1] = ty, context.thread_id[2] = tz;
                    context.dispatch_id[0] = x * compute->group(0) + tx, context.dispatch_id[1] = y * compute->group(1) + ty, context.dispatch_id[2] = z * compute->group(2) + tz;
                    context.group_index = index;
                    compute->function()(context, s.constants);
                  }
            }
        break;
      }
      case RecordedCommand::Type::BeginQuery: {
        const uint32_t kind = cmd.count & 0xFF;
        if (kind > static_cast<uint32_t>(QueryKind::PipelineStatistics) || cmd.unit >= kMaxOcclusionQueries) return false;
        for (const OpenQuery& q : open_queries.view())
          if (q.slot == cmd.unit) return false;   // (a slot is begun once)
        if (!open_queries.push_back({cmd.unit, static_cast<QueryKind>(kind), cmd.count >> 8 & 3, counters})) return false;
        break;
      }
      case RecordedCommand::Type::EndQuery: {
        // (the query of the slot named, or the one begun last)
        uint64_t at = open_queries.count();
        for (uint64_t k = 0; k < open_queries.count(); ++k)
          if (cmd.count == 0 || open_queries[k].slot == cmd.unit) at = k;
        if (at == open_queries.count()) return false;
        const OpenQuery q = open_queries[at];
        if (!open_queries.remove_at(at)) return false;
        const QueryCounters& b = q.at_begin;
        auto statistic = [&](PipelineStatistic which) { return counters.statistics[static_cast<uint32_t>(which)] - b.statistics[static_cast<uint32_t>(which)]; };
        switch (q.kind) {
          case QueryKind::Timestamp: recorder.set_occlusion_result(q.slot, clock_ ? clock_(clock_context_) : (counted_ns_ += 1000)); break;
          case QueryKind::TimestampFrequency: recorder.set_occlusion_result(q.slot, 1000000000u); break;
          case QueryKind::TimestampDisjoint:
            recorder.set_occlusion_result(q.slot, interrupted ? 1u : 0u);
            disjoint_asked = true;
            break;
          case QueryKind::SoPrimitivesWritten: recorder.set_occlusion_result(q.slot, counters.so[2 * q.stream] - b.so[2 * q.stream]); break;
          case QueryKind::SoPrimitivesNeeded: recorder.set_occlusion_result(q.slot, counters.so[2 * q.stream + 1] - b.so[2 * q.stream + 1]); break;
          case QueryKind::Occlusion: recorder.set_occlusion_result(q.slot, counters.samples - b.samples); break;
          case QueryKind::OcclusionPredicate: recorder.set_occlusion_result(q.slot, counters.samples != b.samples ? 1u : 0u); break;
          case QueryKind::IaVertices: recorder.set_occlusion_result(q.slot, statistic(PipelineStatistic::IaVertices)); break;
          case QueryKind::IaPrimitives: recorder.set_occlusion_result(q.slot, statistic(PipelineStatistic::IaPrimitives)); break;
          case QueryKind::VsInvocations: recorder.set_occlusion_result(q.slot, statistic(PipelineStatistic::VsInvocations)); break;
          case QueryKind::PsInvocations: recorder.set_occlusion_result(q.slot, statistic(PipelineStatistic::PsInvocations)); break;
          case QueryKind::PipelineStatistics: {
            uint64_t values[kPipelineStatisticCount];
            for (uint32_t k = 0; k < kPipelineStatisticCount; ++k) values[k] = counters.statistics[k] - b.statistics[k];
            recorder.set_query_values(q.slot, ArrayView<const uint64_t>(values));
            break;
          }
        }
        break;
      }
      case RecordedCommand::Type::BindPipeline:
        s.pipeline = static_cast<const SoftwarePipeline*>(cmd.pipeline);
        break;
      case RecordedCommand::Type::BindVertexBuffer:
        if (cmd.unit >= kMaxVertexBuffers) return false;
        vertex_buffers[cmd.unit] = cmd.buffer;
        vertex_offsets[cmd.unit] = cmd.offset;
        break;
      case RecordedCommand::Type::BindIndexBuffer:
        index_buffer = cmd.buffer;
        index_offset = cmd.offset;
        index_type = cmd.index_type;
        break;
      case RecordedCommand::Type::PushConstants: {
        ConstByteView block;
        if (!pool.try_subview(cmd.constants_offset, cmd.constants_size, &block)) return false;
        for (uint32_t k = 0; k < block.size(); ++k) s.constants_storage[k] = block[k];
        break;
      }
      case RecordedCommand::Type::BindTexture: {
        const auto* image = static_cast<const SoftwareImage*>(cmd.image);
        if (cmd.unit >= kMaxTextureUnits || !image || !(image->usage() & kImageSampled)) return false;   // (a depth image too: kFeature2DepthTextures)
        if (cmd.has_view) {
          if (!s.textures.bind_view(cmd.unit, image, cmd.sampler, cmd.texture_view)) return false;
        } else {
          s.textures.bind(cmd.unit, image, cmd.sampler);
        }
        break;
      }
      case RecordedCommand::Type::SetViewport:
        s.viewport = s.viewports[0] = cmd.viewport;
        s.viewport_count = 1;
        break;
      case RecordedCommand::Type::SetViewportAt:
        if (cmd.unit >= kMaxViewports || cmd.count == 0 || cmd.count > kMaxViewports) return false;
        s.viewports[cmd.unit] = cmd.viewport;
        s.viewport_count = cmd.count;
        if (cmd.unit == 0) s.viewport = cmd.viewport;
        break;
      case RecordedCommand::Type::SetTopology:
        topology = cmd.topology;
        break;
      case RecordedCommand::Type::SetPatchPoints:
        patch_points = cmd.count;
        break;
      case RecordedCommand::Type::SetScissor: {
        if (!in_pass) return false;
        const Extent2D e = s.extent;
        const int64_t x0 = cmd.scissor.x > 0 ? cmd.scissor.x : 0, y0 = cmd.scissor.y > 0 ? cmd.scissor.y : 0;
        int64_t x1 = int64_t{cmd.scissor.x} + cmd.scissor.width, y1 = int64_t{cmd.scissor.y} + cmd.scissor.height;
        if (x1 > e.width) x1 = e.width;
        if (y1 > e.height) y1 = e.height;
        s.scissor = s.scissors[0] = {static_cast<int32_t>(x0), static_cast<int32_t>(y0), x1 > x0 ? static_cast<uint32_t>(x1 - x0) : 0,
                                     y1 > y0 ? static_cast<uint32_t>(y1 - y0) : 0};
        s.scissor_count = 1;
        break;
      }
      case RecordedCommand::Type::SetScissorAt: {
        if (!in_pass || cmd.unit >= kMaxViewports || cmd.count == 0 || cmd.count > kMaxViewports) return false;
        const Extent2D e = s.color->extent();
        const int64_t x0 = cmd.scissor.x > 0 ? cmd.scissor.x : 0, y0 = cmd.scissor.y > 0 ? cmd.scissor.y : 0;
        int64_t x1 = int64_t{cmd.scissor.x} + cmd.scissor.width, y1 = int64_t{cmd.scissor.y} + cmd.scissor.height;
        if (x1 > e.width) x1 = e.width;
        if (y1 > e.height) y1 = e.height;
        s.scissors[cmd.unit] = {static_cast<int32_t>(x0), static_cast<int32_t>(y0), x1 > x0 ? static_cast<uint32_t>(x1 - x0) : 0, y1 > y0 ? static_cast<uint32_t>(y1 - y0) : 0};
        s.scissor_count = cmd.count;
        if (cmd.unit == 0) s.scissor = s.scissors[0];
        break;
      }
      case RecordedCommand::Type::Draw:
      case RecordedCommand::Type::DrawIndexed: {
        if (!in_pass || !s.pipeline) return false;
        const bool indexed = cmd.type == RecordedCommand::Type::DrawIndexed;
        if (indexed && !index_buffer) return false;
        const GraphicsPipelineDesc& pd = s.pipeline->desc();
        if (pd.stencil.enable && (!s.depth || s.depth->stencil().empty())) return false;   // (a stencil test with no stencil plane to test)
        // Every texture the pipeline samples is bound, and none of them is the image being drawn into.
        // (a unit may show other levels or layers of a target than those the pass draws into)
        for (uint32_t unit = 0; unit < pd.texture_count; ++unit) {
          if (!s.textures.image(unit)) return false;
          for (uint32_t n = 0; n <= kMaxColorTargets; ++n) {
            const SoftwareImage* target = n == kMaxColorTargets ? s.depth : n == 0 ? s.color : s.extra_colors[n - 1];
            if (target && s.textures.overlaps(unit, target, s.level[n], s.first_layer[n], s.layer_count[n])) return false;
          }
        }
        if (pd.color_target_count == 0 || pd.color_target_count > kMaxColorTargets || (pd.color_target_count > 1 && !s.extra_colors[pd.color_target_count - 2])) return false;
        // (the vertex buffers the attributes read: each one bound, and the vertices of the draw inside it)
        VertexStreams streams;
        uint32_t slots_used = 0;
        for (uint32_t k = 0; k < pd.attribute_count; ++k) {
          const uint32_t slot = pd.attributes[k].slot;
          if (slot >= kMaxVertexBuffers || !vertex_buffers[slot]) return false;
          slots_used |= 1u << slot;
        }
        if (slots_used == 0 && !vertex_buffers[0]) return false;
        for (uint32_t slot = 0; slot < kMaxVertexBuffers; ++slot)
          if (slots_used >> slot & 1) {
            streams.data[slot] = ConstByteView(vertex_buffers[slot]->map());
            streams.offset[slot] = vertex_offsets[slot];
          }
        const ConstByteView indices = indexed ? ConstByteView(index_buffer->map()) : ConstByteView();
        const uint32_t index_size = index_type == IndexType::Uint16 ? 2 : 4;
        auto vertex_at = [&](uint32_t n, uint32_t instance, ClipVertex* out) {
          int64_t v = n;
          if (indexed) {
            const uint64_t at = index_offset + uint64_t{cmd.first + n} * index_size;
            if (at + index_size > indices.size()) return false;
            v = index_size == 2 ? static_cast<int64_t>(indices[at] | (indices[at + 1] << 8))
                                : static_cast<int64_t>(indices[at] | (indices[at + 1] << 8) | (indices[at + 2] << 16) |
                                                       (uint32_t{indices[at + 3]} << 24));
            v += cmd.vertex_offset;
          } else {
            v += cmd.first;
          }
          if (v < 0) return false;
          // (per-instance attributes: the element instance_element numbers, in the same buffer at the same stride; every element read fits in its buffer)
          for (uint32_t k = 0; k < pd.attribute_count; ++k) {
            const VertexAttribute& at = pd.attributes[k];
            const uint32_t slot = at.slot < kMaxVertexBuffers ? at.slot : 0;
            if (!(slots_used >> slot & 1)) continue;
            const uint32_t stride = slot == 0 ? pd.vertex_stride : pd.slot_strides[slot];
            const uint64_t element = at.per_instance ? instance_element(at, cmd.first_instance, instance) : static_cast<uint64_t>(v);
            if (streams.offset[slot] + (element + 1) * stride > streams.data[slot].size()) return false;
          }
          VertexContext context;
          context.vertex_id = static_cast<uint32_t>(v);
          context.instance_id = cmd.first_instance + instance;
          context.storage = s.storage;
          context.images = s.images;
          context.textures = &s.textures;
          s.count(PipelineStatistic::VsInvocations);
          *out = run_vertex_shader(*s.pipeline, streams, static_cast<uint64_t>(v), cmd.first_instance, instance, s.constants, context);
          return true;
        };
        const uint32_t instances = cmd.instance_count;
        if (instances > 1u << 20) return false;
        const bool patches = topology == PrimitiveTopology::PatchList;
        const SoftwareShaderModule* hull = static_cast<const SoftwareShaderModule*>(pd.hull_shader);
        const SoftwareShaderModule* domain = static_cast<const SoftwareShaderModule*>(pd.domain_shader);
        // (patches need a hull and a domain shader that take that many control points, and those shaders need patches)
        if (patches != (hull != nullptr) || (patches && (patch_points != hull->desc().tess_input_points || patch_points == 0 || patch_points > kMaxPatchControlPoints))) return false;
        if (patches && pd.geometry_shader) return false;   // (a geometry shader after the tessellator is not made)
        const uint32_t per_primitive = patches ? patch_points : topology == PrimitiveTopology::TriangleListAdjacency ? 6 : topology == PrimitiveTopology::LineListAdjacency ? 4 : topology == PrimitiveTopology::TriangleList ? 3 : topology == PrimitiveTopology::LineList ? 2 : 1;
        const bool adjacency = topology == PrimitiveTopology::TriangleListAdjacency || topology == PrimitiveTopology::LineListAdjacency;
        if (adjacency && !pd.geometry_shader) return false;   // (only a geometry shader takes the neighbours)
        const SoftwareShaderModule* geometry = static_cast<const SoftwareShaderModule*>(pd.geometry_shader);
        if (geometry) {
          // (the geometry shader takes the primitives the draw makes)
          const GeometryPrimitive taken = geometry->desc().geometry_input;
          if (taken != (per_primitive == 6 ? GeometryPrimitive::TriangleAdjacency : per_primitive == 4 ? GeometryPrimitive::LineAdjacency : per_primitive == 3 ? GeometryPrimitive::Triangle : per_primitive == 2 ? GeometryPrimitive::Line : GeometryPrimitive::Point)) return false;
        }
        s.count(PipelineStatistic::IaVertices, uint64_t{cmd.count} * instances);
        s.count(PipelineStatistic::IaPrimitives, uint64_t{cmd.count / per_primitive} * instances);
        // (the tessellator's room is taken once for the draw)
        TessellatorHolder tessellator(allocator_, patches);
        if (patches && !tessellator.valid()) return false;
        for (uint32_t instance = 0; instance < instances; ++instance) {
          s.primitive_id = 0;
          for (uint32_t t = 0; t + per_primitive <= cmd.count; t += per_primitive, ++s.primitive_id) {
            ClipVertex v[kMaxPatchControlPoints];
            for (uint32_t k = 0; k < per_primitive; ++k)
              if (!vertex_at(t + k, instance, &v[k])) return false;
            if (patches) {
              run_tessellation(allocator_, s, *tessellator.get(), *hull, *domain, v, per_primitive);
              continue;
            }
            if (geometry) {
              run_geometry_shader(s, *geometry, v, per_primitive);
              continue;
            }
            capture_stream_output_primitive(s, v, topology == PrimitiveTopology::TriangleList ? 3 : topology == PrimitiveTopology::LineList ? 2 : 1);
            if (s.pipeline->desc().stream_output_count && s.pipeline->desc().rasterized_stream < 0) continue;
            if (topology == PrimitiveTopology::TriangleList) draw_triangle(s, v[0], v[1], v[2]);
            else if (topology == PrimitiveTopology::LineList) draw_line(s, v[0], v[1]);
            else draw_point(s, v[0]);
          }
        }
        break;
      }
    }
  }
  for (DrawState::StreamOutputTarget& target : stream_output) remember_filled(target);
  if (disjoint_asked) clock_interrupted_ = false;
  return !in_pass && open_queries.count() == 0;
}

bool SoftwareDevice::present(IImage* image) {
  auto* img = static_cast<SoftwareImage*>(image);
  if (!img || !display_ || !(img->usage() & kImagePresent) || img->format() != Format::B8G8R8A8Unorm) return false;
  const Surface s = img->surface();
  display_->blit(0, 0, s.width, s.height, s.pixels.data(), s.pitch);
  display_->present();
  return true;
}

}  // namespace graphics
