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

#include "Graphics/Graphics3D/IntelGen9/Gen9GraphicsDevice.hpp"
#include <new>
#include "Utility/AllocatedView.hpp"

namespace graphics {

using namespace hardware;
using namespace hardware::intel_gt;
using hardware::intel_gt::gen9_3d::Blend;
using hardware::intel_gt::gen9_3d::CompareFunc;
using hardware::intel_gt::gen9_3d::StencilAction;
using hardware::intel_gt::gen9_3d::StencilBuffer;
using hardware::intel_gt::gen9_3d::DepthBuffer;
using hardware::intel_gt::gen9_3d::DrawState;
using hardware::intel_gt::gen9_3d::Rect;
using hardware::intel_gt::gen9_3d::RenderBatch;
using hardware::intel_gt::gen9_3d::SurfaceFormat;
using hardware::intel_gt::gen9_3d::Vertex;

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

constexpr uint32_t kPitchAlignment = 64;
constexpr uint32_t kMaxDimension = 4096;
constexpr uint32_t kMaxPushedConstantBytes = 128;
constexpr uint32_t kMvpBytes = 64;
constexpr float kGuardBand = 3.0f;       // triangles are clipped to |x|, |y| <= 3 w: window coordinates stay within a few thousand pixels of the viewport
constexpr float kMinW = 1e-6f;
constexpr float kFiniteLimit = 1e30f;
constexpr uint32_t kMaxClipVertices = 10;

uint32_t align_up(uint32_t value, uint32_t alignment) { return (value + alignment - 1) & ~(alignment - 1); }

uint32_t read_word(const uint8_t* b) { return static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) | (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24); }
float read_float(const uint8_t* b) { return __builtin_bit_cast(float, read_word(b)); }
bool finite(float v) { return v == v && v > -kFiniteLimit && v < kFiniteLimit; }

// --- Resources ---------------------------------------------------------------

class Gen9Buffer final : public IBuffer {
 public:
  Gen9Buffer(uint64_t size) : size_(size) {}
  uint64_t size() const override { return size_; }
  // The engine reads through a snooped (write-back) mapping: what the CPU wrote is what it sees, with no flush.
  ByteView map() override { return buffer_.cpu.first(size_); }
  void unmap() override {}
  GpuBuffer buffer_;
  uint32_t address_ = 0;

 private:
  uint64_t size_;
};

class Gen9Image final : public IImage {
 public:
  explicit Gen9Image(const ImageDesc& desc) : desc_(desc) {}
  Extent2D extent() const override { return desc_.extent; }
  Format format() const override { return desc_.format; }
  uint32_t usage() const { return desc_.usage; }
  uint32_t pitch_bytes() const { return pitch_; }
  hardware::intel_gt::gen9_3d::Surface surface() const {
    hardware::intel_gt::gen9_3d::Surface s;
    s.gpu_address = address_;
    s.width = desc_.extent.width;
    s.height = desc_.extent.height;
    s.pitch = pitch_;
    s.format = desc_.format == Format::R8G8B8A8Unorm ? SurfaceFormat::R8G8B8A8Unorm : SurfaceFormat::B8G8R8A8Unorm;
    return s;
  }
  bool is_depth() const { return desc_.format == Format::D32Float; }
  bool has_stencil() const { return desc_.stencil; }
  hardware::intel_gt::gen9_3d::StencilBuffer stencil_buffer() const {
    hardware::intel_gt::gen9_3d::StencilBuffer b;
    b.gpu_address = stencil_address_;
    b.width = desc_.extent.width;
    b.height = desc_.extent.height;
    return b;
  }
  DepthBuffer depth_buffer() const {
    DepthBuffer d;
    d.gpu_address = address_;
    d.width = desc_.extent.width;
    d.height = desc_.extent.height;
    return d;
  }
  bool read_pixels(ArrayView<uint32_t> out) override {
    const uint64_t count = uint64_t{desc_.extent.width} * desc_.extent.height;
    if (is_depth() || buffer_.cpu.empty() || out.size() < count) return false;
    for (uint32_t y = 0; y < desc_.extent.height; ++y) {
      for (uint32_t x = 0; x < desc_.extent.width; ++x) {
        const uint8_t* p = buffer_.cpu.subview(uint64_t{y} * pitch_ + uint64_t{x} * 4, 4).data();
        const uint32_t b0 = p[0], b1 = p[1], b2 = p[2], b3 = p[3];
        // 0xAARRGGBB from the memory order B, G, R, A or R, G, B, A.
        out[uint64_t{y} * desc_.extent.width + x] = desc_.format == Format::R8G8B8A8Unorm ? (b3 << 24) | (b0 << 16) | (b1 << 8) | b2 : (b3 << 24) | (b2 << 16) | (b1 << 8) | b0;
      }
    }
    return true;
  }
  bool write_pixels(ArrayView<const uint32_t> pixels) override {
    const uint64_t count = uint64_t{desc_.extent.width} * desc_.extent.height;
    if (is_depth() || buffer_.cpu.empty() || pixels.size() != count) return false;
    for (uint32_t y = 0; y < desc_.extent.height; ++y) {
      for (uint32_t x = 0; x < desc_.extent.width; ++x) {
        const uint32_t argb = pixels[uint64_t{y} * desc_.extent.width + x];
        const uint8_t a = static_cast<uint8_t>(argb >> 24), r = static_cast<uint8_t>(argb >> 16), g = static_cast<uint8_t>(argb >> 8), b = static_cast<uint8_t>(argb);
        // The memory order B, G, R, A or R, G, B, A (the inverse of read_pixels).
        const uint8_t bytes[4] = {desc_.format == Format::R8G8B8A8Unorm ? r : b, g, desc_.format == Format::R8G8B8A8Unorm ? b : r, a};
        buffer_.cpu.subview(uint64_t{y} * pitch_ + uint64_t{x} * 4, 4).copy_from(ConstByteView(bytes, 4));
      }
    }
    return true;
  }
  GpuBuffer buffer_;
  uint32_t address_ = 0;
  uint32_t pitch_ = 0;
  GpuBuffer stencil_buffer_memory_;   // the stencil plane (W-tiled, opaque), when the image has one
  uint32_t stencil_address_ = 0;

 private:
  ImageDesc desc_;
};

class Gen9ShaderModule final : public IShaderModule {
 public:
  explicit Gen9ShaderModule(const ShaderModuleDesc& desc) : stage_(desc.stage), program_(desc.eu_program) {}
  ShaderStage stage() const override { return stage_; }
  EuProgram program() const { return program_; }

 private:
  ShaderStage stage_;
  EuProgram program_;
};

class Gen9Pipeline final : public IPipeline {
 public:
  Gen9Pipeline(const GraphicsPipelineDesc& desc, uint32_t position_offset, uint32_t color_offset, bool textured)
      : desc_(desc), position_offset_(position_offset), color_offset_(color_offset), textured_(textured) {}
  const GraphicsPipelineDesc& desc() const { return desc_; }
  uint32_t position_offset() const { return position_offset_; }
  // The second attribute: a float4 colour, or for a textured pipeline a float2 texture coordinate.
  uint32_t color_offset() const { return color_offset_; }
  bool textured() const { return textured_; }

 private:
  GraphicsPipelineDesc desc_;
  uint32_t position_offset_, color_offset_;
  bool textured_;
};

// A vertex after the vertex shader: clip-space position and colour.
struct ClipVertex {
  float p[4];   // x, y, z, w
  float c[4];   // r, g, b, a
};

// One step of Sutherland-Hodgman: keeps the part of the polygon where dot(plane, p) >= 0.
uint32_t clip_against(const ClipVertex* in, uint32_t n, ClipVertex* out, const float plane[4], float offset) {
  uint32_t m = 0;
  for (uint32_t i = 0; i < n; ++i) {
    const ClipVertex& cur = in[i];
    const ClipVertex& next = in[(i + 1) % n];
    const float dc = plane[0] * cur.p[0] + plane[1] * cur.p[1] + plane[2] * cur.p[2] + plane[3] * cur.p[3] - offset;
    const float dn = plane[0] * next.p[0] + plane[1] * next.p[1] + plane[2] * next.p[2] + plane[3] * next.p[3] - offset;
    if (dc >= 0 && m < kMaxClipVertices) out[m++] = cur;
    if ((dc >= 0) != (dn >= 0) && m < kMaxClipVertices) {
      const float t = dc / (dc - dn);
      ClipVertex v;
      for (int k = 0; k < 4; ++k) {
        v.p[k] = cur.p[k] + t * (next.p[k] - cur.p[k]);
        v.c[k] = cur.c[k] + t * (next.c[k] - cur.c[k]);
      }
      out[m++] = v;
    }
  }
  return m;
}

}  // namespace

// A texture as bound for a draw: the image and how it is sampled.
struct Gen9GraphicsDevice::TextureBinding {
  const Gen9Image* image = nullptr;
  SamplerDesc sampler;
};

// One draw of a render pass as recorded: what was bound and set when it was issued.
struct Gen9GraphicsDevice::DrawRecord {
  const Gen9Pipeline* pipeline = nullptr;
  Gen9Buffer* vertex_buffer = nullptr;
  uint64_t vertex_offset = 0;
  Gen9Buffer* index_buffer = nullptr;
  uint64_t index_offset = 0;
  IndexType index_type = IndexType::Uint16;
  TextureBinding texture;
  uint8_t constants[kMaxPushedConstantBytes] = {};
  uint32_t constants_size = 0;
  Viewport viewport;
  bool has_scissor = false;
  Rect2D scissor;
  bool indexed = false;
  uint32_t count = 0, first = 0;
  int32_t vertex_adjust = 0;
};

bool Gen9GraphicsDevice::AddressSpace::take(uint32_t bytes, uint32_t* address) {
  if (bytes == 0 || (bytes & 4095)) return false;
  for (uint32_t i = 0; i < count_; ++i) {
    if (ranges_[i].length < bytes) continue;
    *address = ranges_[i].start;
    ranges_[i].start += bytes;
    ranges_[i].length -= bytes;
    if (ranges_[i].length == 0) {
      for (uint32_t k = i; k + 1 < count_; ++k) ranges_[k] = ranges_[k + 1];
      --count_;
    }
    return true;
  }
  return false;
}

void Gen9GraphicsDevice::AddressSpace::give(uint32_t address, uint32_t bytes) {
  // Keeps the list sorted and merges neighbours; if the list is full the range is lost (the address space is 4 GiB, a few ranges leaked are no loss).
  uint32_t at = 0;
  while (at < count_ && ranges_[at].start < address) ++at;
  const bool joins_before = at > 0 && ranges_[at - 1].start + ranges_[at - 1].length == address;
  const bool joins_after = at < count_ && address + bytes == ranges_[at].start;
  if (joins_before && joins_after) {
    ranges_[at - 1].length += bytes + ranges_[at].length;
    for (uint32_t k = at; k + 1 < count_; ++k) ranges_[k] = ranges_[k + 1];
    --count_;
  } else if (joins_before) {
    ranges_[at - 1].length += bytes;
  } else if (joins_after) {
    ranges_[at].start = address;
    ranges_[at].length += bytes;
  } else if (count_ < kMaxRanges) {
    for (uint32_t k = count_; k > at; --k) ranges_[k] = ranges_[k - 1];
    ranges_[at] = {address, bytes};
    ++count_;
  }
}

Gen9GraphicsDevice::~Gen9GraphicsDevice() {
  if (leased_) engine_.release_lease();
  release_view(allocator_, image_);
  release_view(allocator_, staging_);
}

bool Gen9GraphicsDevice::initialize() {
  if (ready()) return true;
  if (!engine_.try_lease()) return false;
  leased_ = true;
  image_ = allocate_view<uint32_t>(allocator_, VcsDriver::kBatchBytes / 4);
  const RenderBatch sized(image_, VcsDriver::kBatchGpuAddress);
  staging_ = image_.empty() ? ArrayView<Vertex>() : allocate_view<Vertex>(allocator_, sized.max_vertices());
  if (image_.empty() || staging_.empty()) {
    release_view(allocator_, image_);
    release_view(allocator_, staging_);
    engine_.release_lease();
    leased_ = false;
    return false;
  }
  return true;
}

bool Gen9GraphicsDevice::allocate_region(uint64_t bytes, GpuBuffer* buffer, uint32_t* address) {
  if (bytes == 0 || bytes > 0x7FFFF000u) return false;
  const uint32_t rounded = align_up(static_cast<uint32_t>(bytes), 4096);
  if (engine_.state() != VcsDriver::State::Ready || !memory_.allocate(rounded, buffer)) return false;
  if (buffer->cpu.size() != rounded || !addresses_.take(rounded, address)) {
    memory_.free(buffer->cpu);
    return false;
  }
  if (!engine_.map_buffer(*buffer, *address)) {
    addresses_.give(*address, rounded);
    memory_.free(buffer->cpu);
    return false;
  }
  return true;
}

void Gen9GraphicsDevice::free_region(const GpuBuffer& buffer, uint32_t address) {
  if (!buffer.valid()) return;
  // After a hang the engine may still be writing to the memory: it is never freed or reused.
  if (engine_.state() != VcsDriver::State::Ready) return;
  engine_.unmap_buffer(buffer, address);
  addresses_.give(address, static_cast<uint32_t>(buffer.cpu.size()));
  memory_.free(buffer.cpu);
}

IBuffer* Gen9GraphicsDevice::create_buffer(uint64_t size, uint32_t usage) {
  (void)usage;
  Gen9Buffer* buffer = make<Gen9Buffer>(allocator_, size);
  if (buffer && !allocate_region(size, &buffer->buffer_, &buffer->address_)) {
    destroy(allocator_, buffer);
    return nullptr;
  }
  return buffer;
}

void Gen9GraphicsDevice::destroy_buffer(IBuffer* buffer) {
  Gen9Buffer* b = static_cast<Gen9Buffer*>(buffer);
  if (!b) return;
  free_region(b->buffer_, b->address_);
  destroy(allocator_, b);
}

IImage* Gen9GraphicsDevice::create_image(const ImageDesc& desc) {
  const uint32_t w = desc.extent.width, h = desc.extent.height;
  if (w == 0 || h == 0 || w > kMaxDimension || h > kMaxDimension) return nullptr;
  const bool depth = desc.format == Format::D32Float;
  if (!depth && desc.format != Format::B8G8R8A8Unorm && desc.format != Format::R8G8B8A8Unorm) return nullptr;
  if (depth != ((desc.usage & kImageDepthAttachment) != 0) || (depth && (desc.usage & (kImageColorAttachment | kImagePresent)))) return nullptr;
  if (desc.stencil && !depth) return nullptr;   // (a stencil plane goes with a depth image)
  Gen9Image* image = make<Gen9Image>(allocator_, desc);
  if (!image) return nullptr;
  // A depth buffer is Y-tiled (rows of tiles of 128 bytes by 32 rows) and opaque to us; a colour image is linear.
  image->pitch_ = depth ? DepthBuffer::pitch_for(w) : align_up(w * 4, kPitchAlignment);
  const uint64_t bytes = depth ? DepthBuffer::bytes_for(w, h) : uint64_t{image->pitch_} * h;
  if (!allocate_region(bytes, &image->buffer_, &image->address_)) {
    destroy(allocator_, image);
    return nullptr;
  }
  if (depth) {
    // Nothing in front: 1.0 in every word, padding included.
    ArrayView<uint32_t> words;
    if (image->buffer_.cpu.try_cast<uint32_t>(&words))
      for (uint32_t& word : words) word = 0x3F800000;
  }
  if (desc.stencil) {
    // The stencil plane is W-tiled and opaque too; all zeros is a stencil of zero in any layout.
    if (!allocate_region(StencilBuffer::bytes_for(w, h), &image->stencil_buffer_memory_, &image->stencil_address_)) {
      free_region(image->buffer_, image->address_);
      destroy(allocator_, image);
      return nullptr;
    }
    image->stencil_buffer_memory_.cpu.fill(0);
  }
  return image;
}

void Gen9GraphicsDevice::destroy_image(IImage* image) {
  Gen9Image* i = static_cast<Gen9Image*>(image);
  if (!i) return;
  free_region(i->buffer_, i->address_);
  free_region(i->stencil_buffer_memory_, i->stencil_address_);
  destroy(allocator_, i);
}

IShaderModule* Gen9GraphicsDevice::create_shader_module(const ShaderModuleDesc& desc) {
  const bool matches = (desc.stage == ShaderStage::Vertex && (desc.eu_program == EuProgram::ColoredVertex || desc.eu_program == EuProgram::TexturedVertex)) ||
                       (desc.stage == ShaderStage::Fragment && (desc.eu_program == EuProgram::ColoredFragment || desc.eu_program == EuProgram::TexturedFragment));
  return matches ? make<Gen9ShaderModule>(allocator_, desc) : nullptr;
}
void Gen9GraphicsDevice::destroy_shader_module(IShaderModule* module) { destroy(allocator_, static_cast<Gen9ShaderModule*>(module)); }

IPipeline* Gen9GraphicsDevice::create_graphics_pipeline(const GraphicsPipelineDesc& desc) {
  const Gen9ShaderModule* vs = static_cast<const Gen9ShaderModule*>(desc.vertex_shader);
  const Gen9ShaderModule* fs = static_cast<const Gen9ShaderModule*>(desc.fragment_shader);
  if (!vs || !fs) return nullptr;
  // The coloured pair (a float3 position and a float4 colour) or the textured pair (a float3 position and a float2 texture coordinate, one texture).
  const bool textured = vs->program() == EuProgram::TexturedVertex && fs->program() == EuProgram::TexturedFragment;
  const bool colored = vs->program() == EuProgram::ColoredVertex && fs->program() == EuProgram::ColoredFragment;
  if (!textured && !colored) return nullptr;
  if (desc.texture_count != (textured ? 1u : 0u)) return nullptr;
  if (desc.attribute_count != 2 || desc.varying_count != 1 || desc.vertex_stride == 0 || desc.vertex_stride > 2048) return nullptr;
  int32_t position = -1, color = -1;
  for (uint32_t i = 0; i < desc.attribute_count; ++i) {
    const VertexAttribute& a = desc.attributes[i];
    if (a.location == 0 && a.format == VertexFormat::Float3) position = static_cast<int32_t>(a.offset);
    else if (a.location == 1 && a.format == (textured ? VertexFormat::Float2 : VertexFormat::Float4)) color = static_cast<int32_t>(a.offset);
  }
  if (position < 0 || color < 0 || position > 2000 || color > 2000) return nullptr;
  return make<Gen9Pipeline>(allocator_, desc, static_cast<uint32_t>(position), static_cast<uint32_t>(color), textured);
}
void Gen9GraphicsDevice::destroy_pipeline(IPipeline* pipeline) { destroy(allocator_, static_cast<Gen9Pipeline*>(pipeline)); }

ICommandBuffer* Gen9GraphicsDevice::create_command_buffer() { return make<CommandRecorder>(allocator_, allocator_); }
void Gen9GraphicsDevice::destroy_command_buffer(ICommandBuffer* commands) { destroy(allocator_, static_cast<CommandRecorder*>(commands)); }

// Runs the triangles staged so far as one job.
bool Gen9GraphicsDevice::run_job(const DrawState& state, uint32_t vertices, const TextureBinding* texture) {
  if (vertices == 0) return true;
  RenderBatch batch(image_, VcsDriver::kBatchGpuAddress);
  if (texture) {
    hardware::intel_gt::gen9_3d::Sampler sampler;
    sampler.linear = texture->sampler.filter == Filter::Linear;
    sampler.repeat_u = texture->sampler.address_u == AddressMode::Repeat;
    sampler.repeat_v = texture->sampler.address_v == AddressMode::Repeat;
    if (!batch.textured_triangles(state, ArrayView<const Vertex>(staging_.data(), vertices), texture->image->surface(), sampler)) return false;
  } else if (!batch.triangles(state, ArrayView<const Vertex>(staging_.data(), vertices), true, nullptr)) {
    return false;
  }
  return engine_.run_batch(ArrayView<const uint32_t>(image_.data(), batch.dwords())) == VcsDriver::Result::Done;
}

// Shades, clips, projects and culls the triangles of one draw on the CPU and runs them on the engine.
bool Gen9GraphicsDevice::run_draw(const DrawRecord& d, const DrawState& base_state, const Extent2D& target) {
  if (d.constants_size < kMvpBytes || !d.vertex_buffer || !d.pipeline) return false;
  const GraphicsPipelineDesc& pd = d.pipeline->desc();

  // The viewport (default: the whole image) as a rectangle in whole pixels.
  float vx = 0, vy = 0, vw = static_cast<float>(target.width), vh = static_cast<float>(target.height);
  if (d.viewport.width != 0 || d.viewport.height != 0) {
    vx = d.viewport.x, vy = d.viewport.y, vw = d.viewport.width, vh = d.viewport.height;
    if (!(vx >= 0 && vx <= kMaxDimension && vy >= 0 && vy <= kMaxDimension && vw > 0 && vw <= kMaxDimension && vh > 0 && vh <= kMaxDimension)) return false;
  }
  DrawState state = base_state;
  state.blend = pd.alpha_blend ? Blend::SourceOver : Blend::None;
  state.depth_test = pd.depth_test;
  state.depth_write = pd.depth_write;
  if (pd.stencil.enable) {
    const StencilState& st = pd.stencil;
    // (the ops of the API in the hardware's numbering; the compare functions are numbered alike)
    auto action = [](StencilOp op) {
      switch (op) {
        case StencilOp::Keep: return StencilAction::Keep;
        case StencilOp::Zero: return StencilAction::Zero;
        case StencilOp::Replace: return StencilAction::Replace;
        case StencilOp::IncrementClamp: return StencilAction::IncrementSaturate;
        case StencilOp::DecrementClamp: return StencilAction::DecrementSaturate;
        case StencilOp::Invert: return StencilAction::Invert;
        case StencilOp::IncrementWrap: return StencilAction::Increment;
        case StencilOp::DecrementWrap: return StencilAction::Decrement;
      }
      return StencilAction::Keep;
    };
    state.stencil_test.enable = true;
    state.stencil_test.func = static_cast<CompareFunc>(st.compare);
    state.stencil_test.fail = action(st.fail_op);
    state.stencil_test.depth_fail = action(st.depth_fail_op);
    state.stencil_test.pass = action(st.pass_op);
    state.stencil_test.compare_mask = st.compare_mask;
    state.stencil_test.write_mask = st.write_mask;
    state.stencil_test.reference = st.reference;
  }
  const float min_depth = d.viewport.width != 0 || d.viewport.height != 0 ? d.viewport.min_depth : 0.0f;
  const float max_depth = d.viewport.width != 0 || d.viewport.height != 0 ? d.viewport.max_depth : 1.0f;
  if (!(min_depth >= 0.0f && max_depth <= 1.0f && min_depth <= max_depth)) return false;
  Rect clip = {static_cast<int32_t>(vx + 0.5f), static_cast<int32_t>(vy + 0.5f), static_cast<int32_t>(vx + vw + 0.5f), static_cast<int32_t>(vy + vh + 0.5f)};
  if (d.has_scissor) {
    const int64_t x1 = d.scissor.x + int64_t{d.scissor.width}, y1 = d.scissor.y + int64_t{d.scissor.height};
    if (d.scissor.x > clip.x0) clip.x0 = d.scissor.x;
    if (d.scissor.y > clip.y0) clip.y0 = d.scissor.y;
    if (x1 < clip.x1) clip.x1 = static_cast<int32_t>(x1);
    if (y1 < clip.y1) clip.y1 = static_cast<int32_t>(y1);
  }
  if (clip.x0 < 0) clip.x0 = 0;
  if (clip.y0 < 0) clip.y0 = 0;
  if (clip.x1 > static_cast<int32_t>(target.width)) clip.x1 = static_cast<int32_t>(target.width);
  if (clip.y1 > static_cast<int32_t>(target.height)) clip.y1 = static_cast<int32_t>(target.height);
  if (clip.empty()) return true;   // nothing of the draw is visible
  state.clip = clip;

  float mvp[16];
  for (uint32_t k = 0; k < 16; ++k) {
    mvp[k] = read_float(d.constants + 4 * k);
    if (!finite(mvp[k])) return false;
  }
  const uint32_t vertex_count = d.count - d.count % 3;   // triangles only
  const ByteView vertices = d.vertex_buffer->map();
  ByteView indices;
  uint32_t index_size = 0;
  if (d.indexed) {
    if (!d.index_buffer) return false;
    index_size = d.index_type == IndexType::Uint16 ? 2 : 4;
    indices = d.index_buffer->map();
    if (d.index_offset % index_size != 0) return false;
  }
  const TextureBinding* texture = d.pipeline->textured() ? &d.texture : nullptr;
  const uint32_t second_bytes = texture ? 8 : 16;   // a texture coordinate or a colour
  const uint64_t attribute_end = d.pipeline->color_offset() + second_bytes > d.pipeline->position_offset() + 12 ? d.pipeline->color_offset() + second_bytes : d.pipeline->position_offset() + 12;

  for (uint32_t t = 0; t < vertex_count; t += 3) {
    ClipVertex poly[2][kMaxClipVertices];
    for (uint32_t k = 0; k < 3; ++k) {
      int64_t index = d.first + t + k;
      if (d.indexed) {
        const uint64_t at = d.index_offset + uint64_t{d.first + t + k} * index_size;
        if (at > indices.size() || index_size > indices.size() - at) return false;
        index = indices[at] | (indices[at + 1] << 8);
        if (index_size == 4) index |= (static_cast<int64_t>(indices[at + 2]) << 16) | (static_cast<int64_t>(indices[at + 3]) << 24);
        index += d.vertex_adjust;
        if (index < 0) return false;
      }
      const uint64_t base = d.vertex_offset + static_cast<uint64_t>(index) * pd.vertex_stride;
      if (base > vertices.size() || attribute_end > vertices.size() - base) return false;
      const uint8_t* pos = vertices.data() + base + d.pipeline->position_offset();
      const uint8_t* col = vertices.data() + base + d.pipeline->color_offset();
      const float x = read_float(pos), y = read_float(pos + 4), z = read_float(pos + 8);
      if (!finite(x) || !finite(y) || !finite(z)) return false;
      // The matrix is column major: clip = M * (x, y, z, 1).
      for (int r = 0; r < 4; ++r) poly[0][k].p[r] = mvp[r] * x + mvp[4 + r] * y + mvp[8 + r] * z + mvp[12 + r];
      for (int r = 0; r < 4; ++r) {
        // A textured vertex carries (u, v) in the first two components of the attribute the kernel reads; the other two are unused.
        poly[0][k].c[r] = texture && r >= 2 ? 0.0f : read_float(col + 4 * r);
        if (!finite(poly[0][k].c[r]) || !finite(poly[0][k].p[r])) return false;
      }
    }
    // Clip against the near plane (z >= 0), w > 0 and the guard band, then project.
    static const float planes[6][4] = {{0, 0, 1, 0}, {0, 0, 0, 1}, {-1, 0, 0, kGuardBand}, {1, 0, 0, kGuardBand}, {0, -1, 0, kGuardBand}, {0, 1, 0, kGuardBand}};
    const float offsets[6] = {0, kMinW, 0, 0, 0, 0};
    uint32_t n = 3, from = 0;
    for (int p = 0; p < 6 && n >= 3; ++p) {
      n = clip_against(poly[from], n, poly[1 - from], planes[p], offsets[p]);
      from = 1 - from;
    }
    if (n < 3) continue;
    const ClipVertex* clipped = poly[from];
    Vertex window[kMaxClipVertices];
    float area = 0;
    for (uint32_t i = 0; i < n; ++i) {
      const float inv_w = 1.0f / clipped[i].p[3];
      window[i].position[0] = vx + (clipped[i].p[0] * inv_w * 0.5f + 0.5f) * vw;
      window[i].position[1] = vy + (clipped[i].p[1] * inv_w * 0.5f + 0.5f) * vh;
      window[i].position[2] = min_depth + clipped[i].p[2] * inv_w * (max_depth - min_depth);
      window[i].position[3] = inv_w;   // (what the clipper would hand the setup stage: the colours are interpolated perspective-correct from it)
      for (int k = 0; k < 4; ++k) window[i].color[k] = clipped[i].c[k];
    }
    for (uint32_t i = 0; i < n; ++i) {
      const Vertex& a = window[i];
      const Vertex& b = window[(i + 1) % n];
      area += a.position[0] * b.position[1] - b.position[0] * a.position[1];
    }
    // (The sum is the cross product the software device uses: with y down, positive is clockwise on screen.)
    if (area == 0) continue;
    const bool clockwise = area > 0;
    const bool front = (pd.front_face == FrontFace::Clockwise) == clockwise;
    if ((pd.cull_mode == CullMode::Back && !front) || (pd.cull_mode == CullMode::Front && front)) continue;
    for (uint32_t i = 1; i + 1 < n; ++i) {
      if (staged_ + 3 > staging_.size()) {
        if (!run_job(state, staged_, texture)) return false;
        staged_ = 0;
      }
      staging_[staged_++] = window[0];
      staging_[staged_++] = window[i];
      staging_[staged_++] = window[i + 1];
    }
  }
  const uint32_t pending = staged_;
  staged_ = 0;
  return run_job(state, pending, texture);
}

bool Gen9GraphicsDevice::run_pass(const RenderPassBegin& pass, const List<DrawRecord>& draws) {
  Gen9Image* color = static_cast<Gen9Image*>(pass.color);
  Gen9Image* depth = static_cast<Gen9Image*>(pass.depth);
  if (!color || color->is_depth() || !(color->usage() & kImageColorAttachment) || !ready() || engine_.state() != VcsDriver::State::Ready) return false;
  if (depth && (!depth->is_depth() || !(depth->usage() & kImageDepthAttachment) || depth->extent().width != color->extent().width || depth->extent().height != color->extent().height))
    return false;
  for (uint64_t i = 0; i < draws.count(); ++i) {
    if (!draws[i].pipeline) return false;
    // A textured draw needs its texture bound, and it is not the image being drawn into (or the depth buffer).
    if (draws[i].pipeline->textured() && (!draws[i].texture.image || draws[i].texture.image == color || draws[i].texture.image == depth)) return false;
    // A stencil test needs a stencil plane in the depth image.
    if (draws[i].pipeline->desc().stencil.enable && (!depth || !depth->has_stencil())) return false;
  }
  DrawState state;
  state.target = color->surface();
  if (!state.target.valid()) return false;
  staged_ = 0;
  if (depth) {
    state.depth = depth->depth_buffer();
    if (depth->has_stencil()) {
      state.stencil = depth->stencil_buffer();
      if (pass.clear_stencil) {
        RenderBatch batch(image_, VcsDriver::kBatchGpuAddress);
        if (!batch.clear_stencil(state, pass.clear_stencil_value) || engine_.run_batch(ArrayView<const uint32_t>(image_.data(), batch.dwords())) != VcsDriver::Result::Done) return false;
      }
    }
    if (pass.clear_depth) {
      RenderBatch batch(image_, VcsDriver::kBatchGpuAddress);
      if (!batch.clear_depth(state, pass.clear_depth_value) || engine_.run_batch(ArrayView<const uint32_t>(image_.data(), batch.dwords())) != VcsDriver::Result::Done) return false;
    }
  }
  if (pass.clear_color) {
    const float rgba[4] = {pass.clear_color_value.x, pass.clear_color_value.y, pass.clear_color_value.z, pass.clear_color_value.w};
    RenderBatch batch(image_, VcsDriver::kBatchGpuAddress);
    const Rect whole = {0, 0, static_cast<int32_t>(color->extent().width), static_cast<int32_t>(color->extent().height)};
    if (!batch.fill_rect(state, whole, rgba) || engine_.run_batch(ArrayView<const uint32_t>(image_.data(), batch.dwords())) != VcsDriver::Result::Done) return false;
  }
  for (uint64_t i = 0; i < draws.count(); ++i)
    if (!run_draw(draws[i], state, color->extent())) return false;
  return true;
}

bool Gen9GraphicsDevice::submit(ICommandBuffer* commands) {
  if (!commands) return false;
  const CommandRecorder& recorder = *static_cast<CommandRecorder*>(commands);
  List<DrawRecord> draws(allocator_);
  DrawRecord current;
  RenderPassBegin pass;
  bool in_pass = false;
  const ConstByteView pool = recorder.constants();

  for (uint64_t i = 0; i < recorder.commands().count(); ++i) {
    const RecordedCommand& cmd = recorder.commands()[i];
    switch (cmd.type) {
      case RecordedCommand::Type::BeginRenderPass:
        if (in_pass || !cmd.pass.color) return false;
        in_pass = true;
        pass = cmd.pass;
        draws.clear();
        current = DrawRecord();  // bindings do not carry over from before the pass
        break;
      case RecordedCommand::Type::BindPipeline: current.pipeline = static_cast<const Gen9Pipeline*>(cmd.pipeline); break;
      case RecordedCommand::Type::BindVertexBuffer: current.vertex_buffer = static_cast<Gen9Buffer*>(cmd.buffer); current.vertex_offset = cmd.offset; break;
      case RecordedCommand::Type::BindIndexBuffer: current.index_buffer = static_cast<Gen9Buffer*>(cmd.buffer); current.index_offset = cmd.offset; current.index_type = cmd.index_type; break;
      case RecordedCommand::Type::BindTexture: {
        const Gen9Image* image = static_cast<const Gen9Image*>(cmd.image);
        if (cmd.unit != 0 || !image || !(image->usage() & kImageSampled) || image->is_depth()) return false;
        current.texture.image = image;
        current.texture.sampler = cmd.sampler;
        break;
      }
      case RecordedCommand::Type::PushConstants: {
        if (cmd.constants_size > kMaxPushedConstantBytes || cmd.constants_offset > pool.size() || cmd.constants_size > pool.size() - cmd.constants_offset) return false;
        for (uint32_t b = 0; b < cmd.constants_size; ++b) current.constants[b] = pool[cmd.constants_offset + b];
        current.constants_size = cmd.constants_size;
        break;
      }
      case RecordedCommand::Type::SetViewport: current.viewport = cmd.viewport; break;
      case RecordedCommand::Type::SetScissor: current.scissor = cmd.scissor; current.has_scissor = true; break;
      case RecordedCommand::Type::Draw:
      case RecordedCommand::Type::DrawIndexed: {
        if (!in_pass || !current.pipeline || !current.vertex_buffer) return false;
        const bool indexed = cmd.type == RecordedCommand::Type::DrawIndexed;
        if (indexed && !current.index_buffer) return false;
        DrawRecord d = current;
        d.indexed = indexed;
        d.count = cmd.count;
        d.first = cmd.first;
        d.vertex_adjust = cmd.vertex_offset;
        if (!draws.push_back(d)) return false;
        break;
      }
      case RecordedCommand::Type::EndRenderPass:
        if (!in_pass) return false;
        in_pass = false;
        if (!run_pass(pass, draws)) return false;
        draws.clear();
        break;
    }
  }
  return !in_pass;
}

bool Gen9GraphicsDevice::present(IImage* image) {
  Gen9Image* img = static_cast<Gen9Image*>(image);
  if (!img || !display_ || !(img->usage() & kImagePresent) || img->format() != Format::B8G8R8A8Unorm) return false;
  ArrayView<uint32_t> pixels;
  if (!img->buffer_.cpu.try_cast<uint32_t>(&pixels)) return false;
  display_->blit(0, 0, img->extent().width, img->extent().height, pixels.data(), img->pitch_bytes() / 4);
  display_->present();
  return true;
}

}  // namespace graphics
