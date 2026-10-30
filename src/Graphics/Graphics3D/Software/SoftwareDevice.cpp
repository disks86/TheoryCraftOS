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
  SoftwareImage(allocators::IAllocator& allocator, const ImageDesc& desc) : allocator_(allocator), desc_(desc) {
    const uint64_t count = uint64_t{desc.extent.width} * desc.extent.height * desc.samples;   // (a multisampled image keeps its samples one after the other for each pixel)
    uint64_t total = count * layer_count();   // (layers: one level each)
    // The levels of a mipmapped image follow level 0 in the same storage, each as tight rows.
    for (uint32_t level = 1; level < desc.mip_levels && level < kMaxMipLevels; ++level) {
      level_offset_[level] = total;
      total += uint64_t{mip_extent(desc.extent.width, level)} * mip_extent(desc.extent.height, level);
    }
    if (desc.format == Format::D32Float) {
      depth_ = allocate_view<float>(allocator, count);
      if (desc.stencil) stencil_ = allocate_view<uint8_t>(allocator, count);
    } else if (is_wide_format(desc.format)) {
      if (desc.mip_levels == 1) hdr_ = allocate_view<Vec4>(allocator, count);
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
  ArrayView<uint32_t> all_samples() const { return color_; }
  uint32_t layer_count() const override { return desc_.kind == TextureKind::Texture2D ? 1 : desc_.kind == TextureKind::TextureCube ? 6 : desc_.layers; }
  TextureKind kind() const { return desc_.kind; }
  bool write_layer_pixels(uint32_t layer, ArrayView<const uint32_t> pixels) override {
    if (color_.empty() || layer >= layer_count() || desc_.mip_levels != 1) return false;
    const Surface s = surface(0, layer);
    if (pixels.size() != uint64_t{s.width} * s.height) return false;
    for (uint64_t i = 0; i < pixels.size(); ++i) s.pixels[i] = s.encode(pixels[i]);
    return true;
  }
  bool read_layer_pixels(uint32_t layer, ArrayView<uint32_t> out) override {
    if (color_.empty() || layer >= layer_count() || desc_.mip_levels != 1) return false;
    const Surface s = surface(0, layer);
    if (out.size() < uint64_t{s.width} * s.height) return false;
    for (uint64_t i = 0; i < uint64_t{s.width} * s.height; ++i) out[i] = s.decode(s.pixels[i]);
    return true;
  }
  Extent2D extent() const override { return desc_.extent; }
  Format format() const override { return desc_.format; }
  uint32_t usage() const { return desc_.usage; }
  bool read_pixels(ArrayView<uint32_t> out) override {
    const uint64_t count = uint64_t{desc_.extent.width} * desc_.extent.height;
    if (color_.empty() || out.size() < count || desc_.samples > 1) return false;   // (a multisampled image is resolved first)
    Surface s = surface();
    if (s.format != Format::R8G8B8A8Unorm && s.format != Format::R8G8B8A8Srgb) return out.copy_from(ArrayView<const uint32_t>(color_.data(), count));   // (stored as it is read: one copy)
    for (uint64_t i = 0; i < count; ++i) out[i] = s.decode(color_[i]);
    return true;
  }
  bool write_pixels(ArrayView<const uint32_t> pixels) override { return desc_.samples == 1 && write_mip_pixels(0, pixels); }
  bool read_pixels_float(ArrayView<Vec4> out) override {
    if (hdr_.empty() || out.size() < hdr_.size()) return false;
    for (uint64_t i = 0; i < hdr_.size(); ++i) out[i] = hdr_[i];
    return true;
  }
  bool write_pixels_float(ArrayView<const Vec4> pixels) override {
    if (hdr_.empty() || pixels.size() != hdr_.size()) return false;
    for (uint64_t i = 0; i < hdr_.size(); ++i) hdr_[i] = store_float(pixels[i]);
    return true;
  }
  // What a pixel of this image keeps of a colour (WidePixels.hpp).
  Vec4 store_float(Vec4 c) const { return quantize_wide_pixel(desc_.format, c); }
  bool write_mip_pixels(uint32_t level, ArrayView<const uint32_t> pixels) override {
    if (color_.empty() || level >= desc_.mip_levels) return false;
    const Surface s = surface(level);
    const uint64_t count = uint64_t{s.width} * s.height;
    if (pixels.size() != count) return false;
    for (uint64_t i = 0; i < count; ++i) s.pixels[i] = s.encode(pixels[i]);
    return true;
  }
  bool generate_mipmaps() override {
    if (color_.empty() || desc_.mip_levels < 2) return false;
    // (the box filter is applied to 0xAARRGGBB values, so the stored order is undone and redone around it)
    for (uint32_t level = 1; level < desc_.mip_levels; ++level) {
      const Surface src = surface(level - 1), dst = surface(level);
      for (uint64_t i = 0; i < uint64_t{dst.width} * dst.height; ++i) {
        const uint32_t y = static_cast<uint32_t>(i / dst.width), x = static_cast<uint32_t>(i % dst.width);
        const uint32_t y0 = 2 * y < src.height ? 2 * y : src.height - 1, y1 = 2 * y + 1 < src.height ? 2 * y + 1 : src.height - 1;
        const uint32_t x0 = 2 * x < src.width ? 2 * x : src.width - 1, x1 = 2 * x + 1 < src.width ? 2 * x + 1 : src.width - 1;
        const uint32_t block[4] = {src.decode(src.pixels[uint64_t{y0} * src.width + x0]), src.decode(src.pixels[uint64_t{y0} * src.width + x1]),
                                   src.decode(src.pixels[uint64_t{y1} * src.width + x0]), src.decode(src.pixels[uint64_t{y1} * src.width + x1])};
        uint32_t averaged = 0;
        mip_box_downsample(ArrayView<const uint32_t>(block, 4), 2, 2, ArrayView<uint32_t>(&averaged, 1), 1, 1);
        dst.pixels[i] = dst.encode(averaged);
      }
    }
    return true;
  }
  Surface surface(uint32_t level = 0, uint32_t layer = 0) const {
    Surface s;
    const uint64_t offset = level == 0 ? uint64_t{layer} * desc_.extent.width * desc_.extent.height : level_offset_[level];
    s.width = mip_extent(desc_.extent.width, level);
    s.height = mip_extent(desc_.extent.height, level);
    if (!hdr_.empty()) s.floats = hdr_;
    else s.pixels = color_.subview(offset, uint64_t{s.width} * s.height);
    s.pitch = s.width * desc_.samples;   // (a multisampled image: the samples of a pixel are neighbours, so the pixel x is at x * samples)
    s.format = desc_.format;
    return s;
  }
  ArrayView<float> depth() const { return depth_; }
  ArrayView<uint8_t> stencil() const { return stencil_; }

 private:
  allocators::IAllocator& allocator_;
  ImageDesc desc_;
  ArrayView<uint32_t> color_;
  ArrayView<Vec4> hdr_;   // the pixels of a float format
  ArrayView<float> depth_;
  ArrayView<uint8_t> stencil_;
  uint64_t level_offset_[kMaxMipLevels] = {};   // (in pixels, from the start of level 0)
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

// The textures bound for the draws being made, and the sampling itself (see SamplerDesc for what the coordinates mean).
class TextureSet final : public ITextureSet {
 public:
  void bind(uint32_t unit, const SoftwareImage* image, const SamplerDesc& sampler) {
    images_[unit] = image;
    samplers_[unit] = sampler;
  }
  const SoftwareImage* image(uint32_t unit) const { return unit < kMaxTextureUnits ? images_[unit] : nullptr; }

  // Whether any bound unit reads mip levels: only then does a pixel need its quad's derivatives.
  bool uses_lod() const {
    for (uint32_t unit = 0; unit < kMaxTextureUnits; ++unit)
      if (images_[unit] && images_[unit]->mip_levels() > 1 && samplers_[unit].mip_filter != MipFilter::None) return true;
    return false;
  }
  // For the derivative passes: the next lookups record their first coordinate pair per unit and answer black.
  void start_recording() const {
    recording_ = true;
    for (uint32_t unit = 0; unit < kMaxTextureUnits; ++unit) recorded_[unit] = false;
  }
  void stop_recording() const { recording_ = false; }
  bool recorded(uint32_t unit, float* u, float* v) const {
    if (unit >= kMaxTextureUnits || !recorded_[unit]) return false;
    *u = record_u_[unit];
    *v = record_v_[unit];
    return true;
  }
  // The level of detail the pixel's lookups of `unit` use, from the coordinates recorded at the quad's anchor and at its right and lower neighbours.
  void set_lod(uint32_t unit, const float anchor[2], const float right[2], const float below[2]) const {
    if (unit >= kMaxTextureUnits || !images_[unit]) return;
    const SamplerDesc& sampler = samplers_[unit];
    const float w = static_cast<float>(images_[unit]->extent().width), h = static_cast<float>(images_[unit]->extent().height);
    const float dudx = (right[0] - anchor[0]) * w, dvdx = (right[1] - anchor[1]) * h, dudy = (below[0] - anchor[0]) * w, dvdy = (below[1] - anchor[1]) * h;
    const float rho_x = dudx * dudx + dvdx * dvdx, rho_y = dudy * dudy + dvdy * dvdy;
    const float rho2 = rho_x > rho_y ? rho_x : rho_y;   // (the square of rho: log2(rho) is half of log2(rho2))
    float lambda = 0.5f * mip_log2(rho2) + sampler.lod_bias;
    if (lambda < sampler.min_lod) lambda = sampler.min_lod;
    if (lambda > sampler.max_lod) lambda = sampler.max_lod;
    lambda_[unit] = lambda == lambda ? lambda : 0.0f;
  }
  void clear_lod() const {
    for (uint32_t unit = 0; unit < kMaxTextureUnits; ++unit) lambda_[unit] = 0.0f;
  }

  Vec4 sample_volume(uint32_t unit, float u, float v, float w) const override {
    const SoftwareImage* image = image_for(unit);
    if (!image) return {0, 0, 0, 1};
    if (image->kind() == TextureKind::Texture2D) return sample(unit, u, v);
    if (recording_) return {0, 0, 0, 1};
    SamplerDesc sampler = samplers_[unit];
    const uint32_t layers = image->layer_count();
    w = unit_coordinate(w);
    if (image->kind() == TextureKind::TextureCube) {
      // The face is the axis the direction is longest along; the point on it is the other two components over that one (the usual table: +X looks along -z with t down -y, ...).
      const float ax = u < 0 ? -u : u, ay = v < 0 ? -v : v, az = w < 0 ? -w : w;
      float major, sc, tc;
      uint32_t face;
      if (ax >= ay && ax >= az) face = u >= 0 ? 0 : 1, major = ax, sc = u >= 0 ? -w : w, tc = -v;
      else if (ay >= az) face = v >= 0 ? 2 : 3, major = ay, sc = u, tc = v >= 0 ? w : -w;
      else face = w >= 0 ? 4 : 5, major = az, sc = w >= 0 ? u : -u, tc = -v;
      if (!(major > 0.0f)) return texel_at(*image, 0, sampler, 0.5f, 0.5f);
      sampler.address_u = sampler.address_v = AddressMode::ClampToEdge;
      return texel_at(*image, face, sampler, (sc / major + 1.0f) * 0.5f, (tc / major + 1.0f) * 0.5f);
    }
    const float depth = static_cast<float>(layers);
    if (image->kind() == TextureKind::Texture2DArray) {
      int64_t layer = floor_to(w + 0.5f);
      layer = layer < 0 ? 0 : layer >= layers ? layers - 1 : layer;
      return texel_at(*image, static_cast<uint32_t>(layer), sampler, u, v);
    }
    // A volume: the slice the point lies in, or the two around it blended.
    auto clamp_layer = [&](int64_t i) { return static_cast<uint32_t>(i < 0 ? 0 : i >= layers ? layers - 1 : i); };
    if (sampler.filter == Filter::Nearest) return texel_at(*image, clamp_layer(floor_to(w * depth)), sampler, u, v);
    const float fz = w * depth - 0.5f;
    const int64_t z0 = floor_to(fz);
    const float tz = fz - static_cast<float>(z0);
    const Vec4 a = texel_at(*image, clamp_layer(z0), sampler, u, v), b = texel_at(*image, clamp_layer(z0 + 1), sampler, u, v);
    return a * (1.0f - tz) + b * tz;
  }

  Vec4 sample(uint32_t unit, float u, float v) const override {
    const SoftwareImage* image = image_for(unit);
    if (!image) return {0, 0, 0, 1};
    if (recording_) {
      if (!recorded_[unit]) {
        recorded_[unit] = true;
        record_u_[unit] = u;
        record_v_[unit] = v;
      }
      return {0, 0, 0, 1};
    }
    return sample_with_lambda(*image, samplers_[unit], u, v, lambda_[unit]);
  }

  // An explicit level of detail: clamped as the derivatives' would be.
  Vec4 sample_lod(uint32_t unit, float u, float v, float lod) const override {
    const SoftwareImage* image = image_for(unit);
    if (!image) return {0, 0, 0, 1};
    if (recording_) return sample(unit, u, v);
    const SamplerDesc& sampler = samplers_[unit];
    float lambda = lod == lod ? lod : 0.0f;
    if (lambda < sampler.min_lod) lambda = sampler.min_lod;
    if (lambda > sampler.max_lod) lambda = sampler.max_lod;
    return sample_with_lambda(*image, sampler, u, v, lambda);
  }

  Vec4 fetch(uint32_t unit, int32_t x, int32_t y, uint32_t level) const override {
    const SoftwareImage* image = image_for(unit);
    if (!image || level >= image->mip_levels()) return {0, 0, 0, 0};
    const Surface surface = image->surface(level);
    if (x < 0 || y < 0 || static_cast<uint32_t>(x) >= surface.width || static_cast<uint32_t>(y) >= surface.height) return {0, 0, 0, 0};
    return texel(surface, x, y);
  }

  Vec4 sample_lod_volume(uint32_t unit, float u, float v, float w, float lod) const override {
    const SoftwareImage* image = image_for(unit);
    if (image && image->kind() == TextureKind::Texture2D) return sample_lod(unit, u, v, lod);
    return sample_volume(unit, u, v, w);
  }
  Vec4 fetch_volume(uint32_t unit, int32_t x, int32_t y, int32_t z, uint32_t level) const override {
    const SoftwareImage* image = image_for(unit);
    if (!image) return {0, 0, 0, 0};
    if (image->kind() == TextureKind::Texture2D) return z == 0 ? fetch(unit, x, y, level) : Vec4{0, 0, 0, 0};
    if (image->kind() == TextureKind::TextureCube || level != 0 || z < 0 || static_cast<uint32_t>(z) >= image->layer_count()) return {0, 0, 0, 0};
    const Surface surface = image->surface(0, static_cast<uint32_t>(z));
    if (x < 0 || y < 0 || static_cast<uint32_t>(x) >= surface.width || static_cast<uint32_t>(y) >= surface.height) return {0, 0, 0, 0};
    return texel(surface, x, y);
  }
  Vec4 fetch_sample(uint32_t unit, int32_t x, int32_t y, uint32_t sample) const override {
    const SoftwareImage* image = image_for(unit);
    if (!image || image->samples() < 2 || sample >= image->samples()) return {0, 0, 0, 0};
    const Surface surface = image->surface();
    if (x < 0 || y < 0 || static_cast<uint32_t>(x) >= surface.width || static_cast<uint32_t>(y) >= surface.height) return {0, 0, 0, 0};
    // (the samples of a pixel are neighbours: the pixel x is at x * samples)
    return texel(surface, static_cast<int64_t>(x) * image->samples() + sample, y);
  }
  bool size_volume(uint32_t unit, uint32_t level, uint32_t* width, uint32_t* height, uint32_t* depth, uint32_t* levels) const override {
    if (!size(unit, level, width, height, levels)) return false;
    const SoftwareImage* image = image_for(unit);
    *depth = image->kind() == TextureKind::Texture3D || image->kind() == TextureKind::Texture2DArray ? image->layer_count() : 1;
    return true;
  }

  bool size(uint32_t unit, uint32_t level, uint32_t* width, uint32_t* height, uint32_t* levels) const override {
    const SoftwareImage* image = image_for(unit);
    if (!image) return false;
    *width = mip_extent(image->extent().width, level);
    *height = mip_extent(image->extent().height, level);
    *levels = image->mip_levels();
    return true;
  }

 private:
  static Vec4 sample_with_lambda(const SoftwareImage& image_ref, const SamplerDesc& sampler, float u, float v, float lambda) {
    const SoftwareImage* image = &image_ref;
    const uint32_t levels = image->mip_levels();
    if (sampler.mip_filter == MipFilter::None || levels < 2) return sample_level(*image, 0, sampler, u, v);
    const float top = static_cast<float>(levels - 1);
    if (lambda > top) lambda = top;
    if (!(lambda > 0.0f)) return sample_level(*image, 0, sampler, u, v);   // (magnification)
    if (sampler.mip_filter == MipFilter::Nearest) {
      uint32_t level = static_cast<uint32_t>(lambda + 0.5f);
      if (level > levels - 1) level = levels - 1;
      return sample_level(*image, level, sampler, u, v);
    }
    const uint32_t l0 = static_cast<uint32_t>(lambda), l1 = l0 + 1 < levels ? l0 + 1 : levels - 1;
    const float fraction = lambda - static_cast<float>(l0);
    const Vec4 a = sample_level(*image, l0, sampler, u, v), b = sample_level(*image, l1, sampler, u, v);
    return a * (1.0f - fraction) + b * fraction;
  }

  static Vec4 texel_at(const SoftwareImage& image, uint32_t layer, const SamplerDesc& sampler, float u, float v) { return sample_level(image, 0, sampler, u, v, layer); }

  static Vec4 sample_level(const SoftwareImage& image, uint32_t level, const SamplerDesc& sampler, float u, float v, uint32_t layer = 0) {
    const Surface surface = image.surface(level, layer);
    const int64_t w = surface.width, h = surface.height;
    if (sampler.filter == Filter::Nearest) {
      return texel(surface, index(floor_to(unit_coordinate(u) * static_cast<float>(w)), w, sampler.address_u), index(floor_to(unit_coordinate(v) * static_cast<float>(h)), h, sampler.address_v));
    }
    // Linear: the four texels around the point, whose centres are half a texel in.
    const float fx = unit_coordinate(u) * static_cast<float>(w) - 0.5f, fy = unit_coordinate(v) * static_cast<float>(h) - 0.5f;
    const int64_t x0 = floor_to(fx), y0 = floor_to(fy);
    const float tx = fx - static_cast<float>(x0), ty = fy - static_cast<float>(y0);
    const int64_t xs[2] = {index(x0, w, sampler.address_u), index(x0 + 1, w, sampler.address_u)};
    const int64_t ys[2] = {index(y0, h, sampler.address_v), index(y0 + 1, h, sampler.address_v)};
    const Vec4 a = texel(surface, xs[0], ys[0]), b = texel(surface, xs[1], ys[0]), c = texel(surface, xs[0], ys[1]), d = texel(surface, xs[1], ys[1]);
    const Vec4 top = a * (1 - tx) + b * tx, bottom = c * (1 - tx) + d * tx;
    return top * (1 - ty) + bottom * ty;
  }
  const SoftwareImage* image_for(uint32_t unit) const { return unit < kMaxTextureUnits ? images_[unit] : nullptr; }
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
    return i < 0 ? 0 : (i >= size ? size - 1 : i);
  }
  static Vec4 texel(const Surface& surface, int64_t x, int64_t y) {
    if (!surface.floats.empty()) return surface.float_row(static_cast<uint32_t>(y))[x];
    const uint32_t argb = surface.decode(surface.row(static_cast<uint32_t>(y))[x]);
    constexpr float k = 1.0f / 255.0f;
    if (is_srgb_format(surface.format)) {   // (the colour channels are decoded to linear values, the alpha is linear)
      const float* table = srgb_to_linear_table();
      return {table[(argb >> 16) & 0xFF], table[(argb >> 8) & 0xFF], table[argb & 0xFF], static_cast<float>(argb >> 24) * k};
    }
    return {static_cast<float>((argb >> 16) & 0xFF) * k, static_cast<float>((argb >> 8) & 0xFF) * k, static_cast<float>(argb & 0xFF) * k, static_cast<float>(argb >> 24) * k};
  }

  const SoftwareImage* images_[kMaxTextureUnits] = {};
  SamplerDesc samplers_[kMaxTextureUnits];
  // The state of the pixel being shaded (the shader sees this set through a const reference).
  mutable bool recording_ = false;
  mutable bool recorded_[kMaxTextureUnits] = {};
  mutable float record_u_[kMaxTextureUnits] = {}, record_v_[kMaxTextureUnits] = {};
  mutable float lambda_[kMaxTextureUnits] = {};
};

// --- Rasterization -------------------------------------------------------------

struct ClipVertex {
  Vec4 position;
  Vec4 varying[kMaxVaryings];
};

inline float lerp(float a, float b, float t) { return a + (b - a) * t; }

ClipVertex mix(const ClipVertex& a, const ClipVertex& b, float t, uint32_t varyings) {
  ClipVertex r;
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

// Everything one draw call needs.
struct DrawState {
  const SoftwarePipeline* pipeline = nullptr;
  SoftwareImage* color = nullptr;
  SoftwareImage* extra_colors[kMaxColorTargets - 1] = {};
  SoftwareImage* depth = nullptr;
  Viewport viewport;
  Rect2D scissor;  // already clamped to the target
  TextureSet textures;
  const uint8_t* constants = nullptr;
  uint8_t constants_storage[kMaxPushConstantBytes] = {};
  uint32_t primitive_id = 0;   // the primitive of the draw being rasterized (SV_PrimitiveID)
  uint32_t sample_count = 1;   // samples per pixel of the pass's images
  uint64_t* samples = nullptr; // the open occlusion query's count: one more for every fragment that passes the stencil and depth tests
  uint64_t* ps_invocations = nullptr;   // one more for every run of the fragment shader (a statistics query)
  // The stream output targets (BindStreamOutput): where each buffer's next record goes. `closed` is set once a primitive did not fit.
  struct StreamOutputTarget {
    ByteView view;
    uint64_t written = 0;
    bool closed = false;
    IBuffer* buffer = nullptr;   // (what it is bound to, to remember how far it got)
    uint64_t base = 0;           // (the offset the view starts at)
  };
  StreamOutputTarget* stream_output = nullptr;   // kMaxStreamOutputBuffers of them (null outside a submit)
  uint64_t* so_counts = nullptr;   // an open stream output statistics query: for each stream the primitives written (2 * stream) and needed (2 * stream + 1)
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

// One factor of the blend equation for one channel (0 red .. 3 alpha) of the colour `s` and the destination `d`.
float blend_factor(BlendFactor f, const float s[4], const float d[4], uint32_t channel) {
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
  }
  return 0.0f;
}

// The pixel after blending `source` into `destination` (both 0xAARRGGBB) by the pipeline's blend state, only the channels of its write mask changed.
uint32_t blend_pixel(const BlendState& b, uint32_t source, uint32_t destination) {
  const Vec4 sv = unpack_color(source), dv = unpack_color(destination);
  const float s[4] = {sv.x, sv.y, sv.z, sv.w}, d[4] = {dv.x, dv.y, dv.z, dv.w};
  float out[4];
  for (uint32_t c = 0; c < 4; ++c) {
    const bool alpha = c == 3;
    const BlendOp op = alpha ? b.alpha_op : b.color_op;
    const float sf = blend_factor(alpha ? b.src_alpha : b.src_color, s, d, c), df = blend_factor(alpha ? b.dst_alpha : b.dst_color, s, d, c);
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
Vec4 blend_float(const BlendState& b, bool over, Vec4 source, Vec4 destination) {
  const float s[4] = {source.x, source.y, source.z, source.w}, d[4] = {destination.x, destination.y, destination.z, destination.w};
  float out[4];
  for (uint32_t c = 0; c < 4; ++c) {
    if (!b.custom) {   // (the pipeline's plain source-over, or no blending)
      out[c] = over ? (c == 3 ? s[3] + d[3] * (1.0f - s[3]) : s[c] * s[3] + d[c] * (1.0f - s[3])) : s[c];
      continue;
    }
    const bool alpha = c == 3;
    const BlendOp op = alpha ? b.alpha_op : b.color_op;
    const float sf = blend_factor(alpha ? b.src_alpha : b.src_color, s, d, c), df = blend_factor(alpha ? b.dst_alpha : b.dst_color, s, d, c);
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

// Stencil test, depth write, blend and store of a shaded fragment of the pixel (x, y); depth_ok is the depth test's verdict.
void write_fragment(const DrawState& s, int32_t x, int32_t y, float z, bool depth_ok, const Vec4* colors, uint32_t sample = 0) {
  const GraphicsPipelineDesc& pd = s.pipeline->desc();
  const ArrayView<float> depth = s.depth ? s.depth->depth() : ArrayView<float>();
  const ArrayView<uint8_t> stencil = s.depth ? s.depth->stencil() : ArrayView<uint8_t>();
  const bool stencil_on = pd.stencil.enable && !stencil.empty();
  const uint32_t samples = s.sample_count;
  const uint64_t index = (uint64_t{static_cast<uint32_t>(y)} * s.color->extent().width + static_cast<uint32_t>(x)) * samples + sample;   // (the sample's place in the depth and stencil planes)
  if (stencil_on) {
    // The stencil test comes first, then the depth test; each outcome has its own op (a fragment the shader discarded changed nothing).
    const StencilState& st = pd.stencil;
    const uint8_t stored = stencil[index];
    auto write = [&](StencilOp op) { stencil[index] = static_cast<uint8_t>((stored & ~st.write_mask) | (stencil_apply(op, stored, st.reference) & st.write_mask)); };
    if (!stencil_compare(st.compare, st.reference & st.compare_mask, stored & st.compare_mask)) { write(st.fail_op); return; }
    if (!depth_ok) { write(st.depth_fail_op); return; }
    write(st.pass_op);
  } else if (!depth_ok) {
    return;
  }
  if (s.samples) ++*s.samples;
  for (uint32_t n = 0; n < pd.color_target_count && n < kMaxColorTargets; ++n) {
    const SoftwareImage* image = n == 0 ? s.color : s.extra_colors[n - 1];
    if (!image) break;
    const Surface surface = image->surface();
    // (the state of this target: its own with independent blending, which has no source-over shorthand)
    const bool own = pd.independent_blend && n > 0;
    const BlendState& blend = own ? pd.target_blend[n - 1] : pd.blend;
    const bool over = !own && !blend.custom && pd.alpha_blend;
    if (!surface.floats.empty()) {
      Vec4* pixel = surface.float_row(static_cast<uint32_t>(y)) + x;
      *pixel = image->store_float(blend_float(blend, over, colors[n], *pixel));
      continue;
    }
    uint32_t* pixel = surface.row(static_cast<uint32_t>(y)) + uint64_t{static_cast<uint32_t>(x)} * samples + sample;
    if (is_srgb_format(surface.format)) {
      // Blending is done on linear values: the stored pixel is decoded, blended with the shader's colour, and the result encoded.
      const uint32_t stored = surface.decode(*pixel);
      const float* table = srgb_to_linear_table();
      const Vec4 destination = {table[(stored >> 16) & 0xFF], table[(stored >> 8) & 0xFF], table[stored & 0xFF], static_cast<float>(stored >> 24) * (1.0f / 255.0f)};
      Vec4 linear = blend_float(blend, over, colors[n], destination);
      *pixel = surface.encode(pack_srgb(linear));
      continue;
    }
    uint32_t out = pack_color(colors[n]);
    if (blend.custom) out = blend_pixel(blend, out, surface.decode(*pixel));
    else if (over) out = blend_over(out, surface.decode(*pixel));
    *pixel = surface.encode(out);
  }
  if (pd.depth_write && !depth.empty()) depth[index] = z;
}

// Runs the pipeline's fragment shader; false if it discarded the pixel.
bool run_fragment_shader(const DrawState& s, const Vec4* varying, const FragmentContext& context, Vec4* color) {
  if (s.ps_invocations) ++*s.ps_invocations;
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

void raster_triangle(const DrawState& s, const ScreenVertex& a, const ScreenVertex& b, const ScreenVertex& c,
                     bool front_facing_pass) {
  const GraphicsPipelineDesc& pd = s.pipeline->desc();
  (void)front_facing_pass;
  float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);  // y down: > 0 is clockwise on screen
  if (area == 0) return;
  const bool clockwise = area > 0;
  const bool front = (pd.front_face == FrontFace::Clockwise) == clockwise;
  if (pd.cull_mode == CullMode::Back && !front) return;
  if (pd.cull_mode == CullMode::Front && front) return;

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
  const ArrayView<float> depth = s.depth ? s.depth->depth() : ArrayView<float>();
  const ArrayView<uint8_t> stencil = s.depth ? s.depth->stencil() : ArrayView<uint8_t>();
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
  const bool trim = fx0 > -1e5f && fx1 < 1e5f && fy0 > -1e5f && fy1 < 1e5f;
  struct EdgeLine {
    float vx, vy, dx, dy;
  };
  const EdgeLine edges[3] = {{v1->x, v1->y, e0dx, e0dy}, {v2->x, v2->y, e1dx, e1dy}, {v0->x, v0->y, e2dx, e2dy}};
  const uint32_t target_width = s.color->extent().width;

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
      if (sample_count > 1 && pd.multisample) {
        const SamplePosition* positions = sample_positions(sample_count);
        for (uint32_t k = 0; k < sample_count; ++k) {
          const float sx = px + static_cast<float>(positions[k].x) * (1.0f / 16.0f), sy = py + static_cast<float>(positions[k].y) * (1.0f / 16.0f);
          const float a0 = -((sx - v1->x) * e0dy - (sy - v1->y) * e0dx), a1 = -((sx - v2->x) * e1dy - (sy - v2->y) * e1dx), a2 = -((sx - v0->x) * e2dy - (sy - v0->y) * e2dx);
          if (!weights_inside(a0, a1, a2)) continue;
          covered |= 1u << k;
          sample_z[k] = (a0 * v0->z + a1 * v1->z + a2 * v2->z) * inv_area;
        }
        if (!covered) continue;
      } else {
        if (!weights_inside(l0, l1, l2)) continue;
        covered = (1u << sample_count) - 1;
        const float centre_z = (l0 * v0->z + l1 * v1->z + l2 * v2->z) * inv_area;
        for (uint32_t k = 0; k < sample_count; ++k) sample_z[k] = centre_z;
      }
      const float b0 = l0 * inv_area, b1 = l1 * inv_area, b2 = l2 * inv_area;

      const uint64_t index = uint64_t{static_cast<uint32_t>(y)} * target_width + static_cast<uint32_t>(x);
      const float z = b0 * v0->z + b1 * v1->z + b2 * v2->z;
      uint32_t depth_pass = covered;   // (the samples that pass the depth test)
      if (pd.depth_test && !depth.empty())
        for (uint32_t k = 0; k < sample_count; ++k)
          if ((covered >> k & 1) && !depth_passes(pd.depth_compare, sample_z[k], depth[index * sample_count + k])) depth_pass &= ~(1u << k);
      if (!depth_pass && !stencil_on) continue;   // (with a stencil test the depth result is needed after the shader)

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
        float coord[3][kMaxTextureUnits][2] = {};
        bool have[3][kMaxTextureUnits] = {};
        for (int k = 0; k < 3; ++k) {
          Vec4 quad_varying[kMaxVaryings];
          varyings_at(xs[k], ys[k], quad_varying);
          Vec4 ignored[kMaxColorTargets];
          FragmentContext quad_context;
          quad_context.position = {xs[k], ys[k], z, 1.0f};
          quad_context.front_facing = front;
          quad_context.primitive_id = s.primitive_id;
          s.textures.start_recording();
          run_fragment_shader(s, quad_varying, quad_context, ignored);
          s.textures.stop_recording();
          for (uint32_t unit = 0; unit < kMaxTextureUnits; ++unit) have[k][unit] = s.textures.recorded(unit, &coord[k][unit][0], &coord[k][unit][1]);
        }
        s.textures.clear_lod();
        for (uint32_t unit = 0; unit < kMaxTextureUnits; ++unit)
          if (have[0][unit] && have[1][unit] && have[2][unit]) s.textures.set_lod(unit, coord[0][unit], coord[1][unit], coord[2][unit]);
      }
      FragmentContext context;
      context.position = {px, py, z, b0 * v0->inv_w + b1 * v1->inv_w + b2 * v2->inv_w};
      context.front_facing = front;
      context.primitive_id = s.primitive_id;
      if (!run_fragment_shader(s, varying, context, color)) continue;
      if (pd.alpha_to_coverage && pd.multisample && sample_count > 1) {
        // (the first round(alpha * samples) samples of the pixel keep what they covered)
        const float a = color[0].w;
        uint32_t keep = !(a > 0.0f) ? 0 : a >= 1.0f ? sample_count : static_cast<uint32_t>(a * static_cast<float>(sample_count) + 0.5f);
        if (keep > sample_count) keep = sample_count;
        covered &= (1u << keep) - 1;
      }
      for (uint32_t k = 0; k < sample_count; ++k)
        if (covered >> k & 1) write_fragment(s, x, y, sample_z[k], (depth_pass >> k & 1) != 0, color, k);
    }
  }
}

// A screen-space point or line fragment: the same shader, depth, stencil and blend as a triangle's, from varyings interpolated perspective-correct along the line.
void fragment_at(const DrawState& s, int32_t x, int32_t y, float z, float inv_w, const Vec4* varying) {
  const GraphicsPipelineDesc& pd = s.pipeline->desc();
  const Surface target = s.color->surface();
  if (x < s.scissor.x || y < s.scissor.y || x >= static_cast<int64_t>(s.scissor.x) + s.scissor.width || y >= static_cast<int64_t>(s.scissor.y) + s.scissor.height) return;
  if (x < 0 || y < 0 || static_cast<uint32_t>(x) >= target.width || static_cast<uint32_t>(y) >= target.height) return;
  const ArrayView<float> depth = s.depth ? s.depth->depth() : ArrayView<float>();
  const uint64_t index = uint64_t{static_cast<uint32_t>(y)} * s.color->extent().width + static_cast<uint32_t>(x);
  const uint32_t sample_count = s.sample_count;   // (a line or point covers every sample of its pixel)
  uint32_t depth_pass = (1u << sample_count) - 1;
  if (pd.depth_test && !depth.empty())
    for (uint32_t k = 0; k < sample_count; ++k)
      if (!depth_passes(pd.depth_compare, z, depth[index * sample_count + k])) depth_pass &= ~(1u << k);
  if (!depth_pass && !(pd.stencil.enable && s.depth && !s.depth->stencil().empty())) return;
  FragmentContext context;
  context.position = {static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f, z, inv_w};
  context.front_facing = true;
  context.primitive_id = s.primitive_id;
  Vec4 color[kMaxColorTargets];
  if (!run_fragment_shader(s, varying, context, color)) return;
  for (uint32_t k = 0; k < sample_count; ++k) write_fragment(s, x, y, z, (depth_pass >> k & 1) != 0, color, k);
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

// One pixel for a point (the pixel its position falls in).
void draw_point(const DrawState& s, const ClipVertex& a) {
  const uint32_t varyings = s.pipeline->desc().varying_count;
  if (a.position.z < 0 || !(a.position.w > 1e-9f)) return;
  const ScreenVertex v = to_screen(s, a, varyings);
  if (!(v.x > -1e6f && v.x < 1e6f && v.y > -1e6f && v.y < 1e6f)) return;
  const int32_t x = static_cast<int32_t>(v.x < 0 ? v.x - 1 : v.x), y = static_cast<int32_t>(v.y < 0 ? v.y - 1 : v.y);   // (floor, as far as the pixel it falls in goes)
  fragment_at(s, x, y, v.z, v.inv_w, v.varying);
}

// A line one pixel wide: a step per pixel along the longer axis, the other coordinate and the varyings interpolated (perspective-correct) along it.
void draw_line(const DrawState& s, const ClipVertex& a, const ClipVertex& b) {
  const uint32_t varyings = s.pipeline->desc().varying_count;
  ClipVertex p = a, q = b;
  if (p.position.z < 0 && q.position.z < 0) return;
  if (p.position.z < 0) p = mix(p, q, p.position.z / (p.position.z - q.position.z), varyings);   // (the near plane)
  else if (q.position.z < 0) q = mix(p, q, p.position.z / (p.position.z - q.position.z), varyings);
  if (!(p.position.w > 1e-9f) || !(q.position.w > 1e-9f)) return;
  const ScreenVertex v0 = to_screen(s, p, varyings), v1 = to_screen(s, q, varyings);
  const float limit = 1e6f;
  if (!(v0.x > -limit && v0.x < limit && v0.y > -limit && v0.y < limit && v1.x > -limit && v1.x < limit && v1.y > -limit && v1.y < limit)) return;
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
    fragment_at(s, static_cast<int32_t>(x < 0 ? x - 1 : x), static_cast<int32_t>(y < 0 ? y - 1 : y), z, (1.0f - t) * v0.inv_w + t * v1.inv_w, varying);
  }
}

// Clips a clip-space triangle against the near plane (z >= 0), divides by w and
// rasterizes the 1 or 2 triangles that remain.
void draw_triangle(const DrawState& s, const ClipVertex& a, const ClipVertex& b, const ClipVertex& c) {
  const uint32_t varyings = s.pipeline->desc().varying_count;
  ClipVertex in[3] = {a, b, c};
  ClipVertex poly[4];
  uint32_t n = 0;
  for (uint32_t i = 0; i < 3; ++i) {
    const ClipVertex& cur = in[i];
    const ClipVertex& next = in[(i + 1) % 3];
    const bool cur_in = cur.position.z >= 0, next_in = next.position.z >= 0;
    if (cur_in) poly[n++] = cur;
    if (cur_in != next_in) {
      const float t = cur.position.z / (cur.position.z - next.position.z);
      poly[n++] = mix(cur, next, t, varyings);
    }
  }
  if (n < 3) return;

  ScreenVertex sv[4];
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
  for (uint32_t i = 1; i + 1 < n; ++i) raster_triangle(s, sv[0], sv[i], sv[i + 1], true);
}

ClipVertex run_vertex_shader(const SoftwarePipeline& pipeline, ConstByteView vertex_data, uint64_t byte_offset, uint64_t instance_byte_offset,
                             const uint8_t* constants, const VertexContext& context) {
  const GraphicsPipelineDesc& pd = pipeline.desc();
  Vec4 attributes[kMaxVertexAttributes];
  for (Vec4& a : attributes) a = {0, 0, 0, 1};
  for (uint32_t i = 0; i < pd.attribute_count; ++i) {
    const VertexAttribute& at = pd.attributes[i];
    if (at.location >= kMaxVertexAttributes) continue;
    const uint32_t components = static_cast<uint32_t>(at.format) + 1;
    float v[4] = {0, 0, 0, 1};
    const uint64_t base = at.per_instance ? instance_byte_offset : byte_offset;
    for (uint32_t k = 0; k < components; ++k) v[k] = read_float(vertex_data, base + at.offset + 4 * k);
    attributes[at.location] = {v[0], v[1], v[2], v[3]};
  }
  ShaderIo io;
  if (pipeline.vertex_ex()) pipeline.vertex_ex()(attributes, constants, context, &io);
  else pipeline.vertex()(attributes, constants, &io);
  ClipVertex out;
  out.position = io.position;
  for (uint32_t i = 0; i < kMaxVaryings; ++i) out.varying[i] = io.varying[i];
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
  if (s.so_counts) ++s.so_counts[2 * stream + 1];
  if (!used_buffers) return false;
  bool overflow = false;
  for (uint32_t b = 0; b < kMaxStreamOutputBuffers; ++b)
    if ((used_buffers >> b & 1) && (s.stream_output[b].closed || s.stream_output[b].written + uint64_t{count} * pd.stream_output_stride[b] > s.stream_output[b].view.size())) overflow = true;
  if (overflow) {
    for (uint32_t b = 0; b < kMaxStreamOutputBuffers; ++b)
      if (used_buffers >> b & 1) s.stream_output[b].closed = true;
    return false;
  }
  if (s.so_counts) ++s.so_counts[2 * stream];
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
    if (!used_buffers && !s.so_counts) continue;
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

void run_geometry_instance(const DrawState& s, const ShaderModuleDesc& md, const GeometryInputData& input) {
  GeometryOutputData output;
  md.geometry_function(input, s.constants, &output);
  if (output.count > md.geometry_max_vertices) output.count = md.geometry_max_vertices;
  capture_stream_output(s, md, output);
  // (with stream output only the stream that is rasterized is drawn, if any)
  const int32_t drawn_stream = s.pipeline->desc().stream_output_count ? s.pipeline->desc().rasterized_stream : 0;
  if (drawn_stream < 0) return;
  auto clip = [&](uint32_t i) {
    ClipVertex c;
    c.position = output.vertex[i].position;
    for (uint32_t k = 0; k < kMaxVaryings; ++k) c.varying[k] = output.vertex[i].varying[k];
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
      if (odd) draw_triangle(s, clip(i - 1), clip(i - 2), clip(i));
      else draw_triangle(s, clip(i - 2), clip(i - 1), clip(i));
    }
  }
}

void run_geometry_shader(const DrawState& s, const SoftwareShaderModule& module, const ClipVertex* v, uint32_t count) {
  const ShaderModuleDesc& md = module.desc();
  GeometryInputData input;
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
void run_tessellation(const DrawState& s, Tessellator& tessellator, const SoftwareShaderModule& hull, const SoftwareShaderModule& domain, const ClipVertex* v, uint32_t count) {
  const ShaderModuleDesc& hd = hull.desc();
  HullInputData input;
  input.control_points = count;
  input.primitive_id = s.primitive_id;
  for (uint32_t k = 0; k < count && k < kMaxPatchControlPoints; ++k) {
    input.position[k] = v[k].position;
    for (uint32_t i = 0; i < kMaxVaryings; ++i) input.varying[k][i] = v[k].varying[i];
  }
  HullOutputData patch;
  hd.hull_function(input, s.constants, &patch);
  if (!tessellator.tessellate(hd.tess_domain, hd.tess_partitioning, hd.tess_output, patch.outer, patch.inner)) return;
  const DomainShaderFn ds = domain.desc().domain_function;
  const bool triangles = hd.tess_domain == TessDomain::Triangle;
  const ArrayView<const Tessellator::Point> points = tessellator.points();
  const ArrayView<const uint16_t> indices = tessellator.indices();
  auto vertex = [&](uint32_t index) {
    const Tessellator::Point p = points[index];
    ShaderIo io;
    ds(patch, p.u, p.v, triangles ? 1.0f - p.u - p.v : 0.0f, s.constants, &io);
    ClipVertex c;
    c.position = io.position;
    for (uint32_t k = 0; k < kMaxVaryings; ++k) c.varying[k] = io.varying[k];
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
  if (!desc.extent.width || !desc.extent.height || desc.format == Format::Undefined) return nullptr;
  // Levels need a colour image to sample, and no more than the extent has.
  if (desc.mip_levels == 0 || desc.mip_levels > mip_level_limit(desc.extent) || desc.mip_levels > kMaxMipLevels || (desc.mip_levels > 1 && (desc.format == Format::D32Float || !(desc.usage & kImageSampled)))) return nullptr;
  if (desc.samples != 1) {
    // (a multisampled image is an 8-bit colour or a depth attachment, alone, of one level)
    const bool depth = desc.format == Format::D32Float;
    if ((desc.samples != 2 && desc.samples != 4 && desc.samples != 8) || desc.mip_levels != 1 || desc.kind != TextureKind::Texture2D || is_wide_format(desc.format) ||
        (depth ? desc.usage != kImageDepthAttachment : (desc.usage & ~kImageSampled) != kImageColorAttachment))
      return nullptr;
  }
  if (desc.kind != TextureKind::Texture2D) {
    // (layered images are sampled only: one level, 8-bit colour)
    const uint32_t layers = desc.kind == TextureKind::TextureCube ? 6 : desc.layers;
    if (desc.usage != kImageSampled || desc.mip_levels != 1 || layers == 0 || layers > kMaxTextureLayers || desc.format == Format::D32Float || is_wide_format(desc.format) ||
        (desc.kind == TextureKind::TextureCube && desc.extent.width != desc.extent.height))
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
  if (desc.texture_count > kMaxTextureUnits || (!fs->desc().fragment_function_ex && (desc.texture_count != 0) != textured)) return nullptr;
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
  // (how far each stream output buffer got, for the next bind that appends)
  auto remember_filled = [](const DrawState::StreamOutputTarget& target) {
    if (target.buffer) target.buffer->set_stream_output_filled(target.base + target.written);
  };
  const SoftwareComputePipeline* compute = nullptr;
  ByteView storage[kMaxStorageBuffers];
  DrawState::StreamOutputTarget stream_output[kMaxStreamOutputBuffers];
  uint64_t query_samples = 0;
  uint32_t query_slot = 0;
  QueryKind query_kind = QueryKind::Occlusion;
  uint32_t query_stream = 0;
  uint64_t so_counts[2 * kMaxGeometryStreams] = {};
  uint64_t stat_ia_vertices = 0, stat_ia_primitives = 0, stat_vs = 0, stat_ps = 0;   // (what the draws since the query began did)
  bool query_open = false;
  IBuffer* vertex_buffer = nullptr;
  uint64_t vertex_offset = 0;
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
        const Extent2D e = s.color->extent();
        if (s.depth && (s.depth->extent().width != e.width || s.depth->extent().height != e.height)) return false;
        for (SoftwareImage*& extra : s.extra_colors) extra = nullptr;
        for (uint32_t n = 0; n + 1 < kMaxColorTargets && cmd.pass.extra_colors[n]; ++n) {
          SoftwareImage* image = static_cast<SoftwareImage*>(cmd.pass.extra_colors[n]);
          if (!(image->usage() & kImageColorAttachment) || image->format() == Format::D32Float || image == s.color || image == s.depth) return false;
          if (image->extent().width != e.width || image->extent().height != e.height || image->samples() != s.sample_count) return false;
          for (uint32_t k = 0; k < n; ++k)
            if (s.extra_colors[k] == image) return false;
          s.extra_colors[n] = image;
        }
        if (cmd.pass.clear_color) {
          for (uint32_t n = 0; n < kMaxColorTargets; ++n) {
            const SoftwareImage* image = n == 0 ? s.color : s.extra_colors[n - 1];
            if (!image) break;
            const Surface surface = image->surface();
            if (!surface.floats.empty()) {
              for (Vec4& c : surface.floats) c = image->store_float(cmd.pass.clear_color_value);
              continue;
            }
            const uint32_t v = surface.encode(is_srgb_format(surface.format) ? pack_srgb(cmd.pass.clear_color_value) : pack_color(cmd.pass.clear_color_value));
            image->all_samples().fill(v);   // (every sample of every pixel; the rows of a plain image are tight)
          }
        }
        if (s.depth && cmd.pass.clear_depth) s.depth->depth().fill(cmd.pass.clear_depth_value);
        if (s.depth && cmd.pass.clear_stencil) s.depth->stencil().fill(cmd.pass.clear_stencil_value);
        s.viewport = {0, 0, static_cast<float>(e.width), static_cast<float>(e.height), 0, 1};
        s.scissor = {0, 0, e.width, e.height};
        s.pipeline = nullptr;
        in_pass = true;
        break;
      }
      case RecordedCommand::Type::EndRenderPass:
        if (query_open) return false;
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
        if (mode != ResolveMode::Average) return false;   // (a colour image is averaged or takes sample 0)
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
      case RecordedCommand::Type::BindStorageBuffer: {
        if (in_pass || cmd.unit >= kMaxStorageBuffers || !cmd.buffer) return false;
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
        ComputeContext context;
        for (uint32_t k = 0; k < kMaxStorageBuffers; ++k) context.buffers[k] = storage[k];
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
      case RecordedCommand::Type::BeginQuery:
        if (!in_pass || query_open) return false;
        if ((cmd.count & 0xFF) > static_cast<uint32_t>(QueryKind::TimestampDisjoint)) return false;
        query_open = true;
        query_slot = cmd.unit;
        query_kind = static_cast<QueryKind>(cmd.count & 0xFF);
        query_stream = cmd.count >> 8 & 3;
        query_samples = stat_ia_vertices = stat_ia_primitives = stat_vs = stat_ps = 0;
        for (uint64_t& n : so_counts) n = 0;
        s.samples = &query_samples;
        s.ps_invocations = &stat_ps;
        s.so_counts = so_counts;
        break;
      case RecordedCommand::Type::EndQuery:
        if (!query_open) return false;
        query_open = false;
        s.samples = nullptr;
        s.ps_invocations = nullptr;
        s.so_counts = nullptr;
        if (query_kind == QueryKind::Timestamp) {
          recorder.set_occlusion_result(query_slot, clock_ ? clock_(clock_context_) : (counted_ns_ += 1000));
          break;
        }
        if (query_kind == QueryKind::TimestampFrequency || query_kind == QueryKind::TimestampDisjoint) {
          recorder.set_occlusion_result(query_slot, query_kind == QueryKind::TimestampFrequency ? 1000000000u : 0u);   // (nanoseconds, and a clock nothing interrupts)
          break;
        }
        if (query_kind == QueryKind::SoPrimitivesWritten || query_kind == QueryKind::SoPrimitivesNeeded) {
          recorder.set_occlusion_result(query_slot, so_counts[2 * query_stream + (query_kind == QueryKind::SoPrimitivesNeeded ? 1 : 0)]);
          break;
        }
        recorder.set_occlusion_result(query_slot, query_kind == QueryKind::Occlusion ? query_samples : query_kind == QueryKind::OcclusionPredicate ? (query_samples ? 1u : 0u) : query_kind == QueryKind::IaVertices ? stat_ia_vertices
                                                                                                                              : query_kind == QueryKind::IaPrimitives ? stat_ia_primitives : query_kind == QueryKind::VsInvocations ? stat_vs : stat_ps);
        break;
      case RecordedCommand::Type::BindPipeline:
        s.pipeline = static_cast<const SoftwarePipeline*>(cmd.pipeline);
        break;
      case RecordedCommand::Type::BindVertexBuffer:
        vertex_buffer = cmd.buffer;
        vertex_offset = cmd.offset;
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
        if (cmd.unit >= kMaxTextureUnits || !image || !(image->usage() & kImageSampled) || image->format() == Format::D32Float) return false;
        s.textures.bind(cmd.unit, image, cmd.sampler);
        break;
      }
      case RecordedCommand::Type::SetViewport:
        s.viewport = cmd.viewport;
        break;
      case RecordedCommand::Type::SetTopology:
        topology = cmd.topology;
        break;
      case RecordedCommand::Type::SetPatchPoints:
        patch_points = cmd.count;
        break;
      case RecordedCommand::Type::SetScissor: {
        if (!in_pass) return false;
        const Extent2D e = s.color->extent();
        const int64_t x0 = cmd.scissor.x > 0 ? cmd.scissor.x : 0, y0 = cmd.scissor.y > 0 ? cmd.scissor.y : 0;
        int64_t x1 = int64_t{cmd.scissor.x} + cmd.scissor.width, y1 = int64_t{cmd.scissor.y} + cmd.scissor.height;
        if (x1 > e.width) x1 = e.width;
        if (y1 > e.height) y1 = e.height;
        s.scissor = {static_cast<int32_t>(x0), static_cast<int32_t>(y0), x1 > x0 ? static_cast<uint32_t>(x1 - x0) : 0,
                     y1 > y0 ? static_cast<uint32_t>(y1 - y0) : 0};
        break;
      }
      case RecordedCommand::Type::Draw:
      case RecordedCommand::Type::DrawIndexed: {
        if (!in_pass || !s.pipeline || !vertex_buffer) return false;
        const bool indexed = cmd.type == RecordedCommand::Type::DrawIndexed;
        if (indexed && !index_buffer) return false;
        const GraphicsPipelineDesc& pd = s.pipeline->desc();
        if (pd.stencil.enable && (!s.depth || s.depth->stencil().empty())) return false;   // (a stencil test with no stencil plane to test)
        // Every texture the pipeline samples is bound, and none of them is the image being drawn into.
        for (uint32_t unit = 0; unit < pd.texture_count; ++unit) {
          const SoftwareImage* bound = s.textures.image(unit);
          if (!bound || bound == s.color || bound == s.depth) return false;
          for (const SoftwareImage* extra : s.extra_colors)
            if (bound == extra) return false;
        }
        if (pd.color_target_count == 0 || pd.color_target_count > kMaxColorTargets || (pd.color_target_count > 1 && !s.extra_colors[pd.color_target_count - 2])) return false;
        const ConstByteView vertices = ConstByteView(vertex_buffer->map());
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
          const uint64_t byte = vertex_offset + static_cast<uint64_t>(v) * pd.vertex_stride;
          if (byte + pd.vertex_stride > vertices.size()) return false;
          // (per-instance attributes: the element numbered by the instance, in the same buffer at the same stride)
          const uint64_t instance_byte = vertex_offset + (uint64_t{cmd.first_instance} + instance) * pd.vertex_stride;
          VertexContext context;
          context.vertex_id = static_cast<uint32_t>(v);
          context.instance_id = cmd.first_instance + instance;
          ++stat_vs;
          *out = run_vertex_shader(*s.pipeline, vertices, byte, instance_byte, s.constants, context);
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
        stat_ia_vertices += uint64_t{cmd.count} * instances;
        stat_ia_primitives += uint64_t{cmd.count / per_primitive} * instances;
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
              run_tessellation(s, *tessellator.get(), *hull, *domain, v, per_primitive);
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
  return !in_pass;
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
