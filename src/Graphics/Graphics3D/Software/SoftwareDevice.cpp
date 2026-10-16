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

// --- Resources ---------------------------------------------------------------

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
    const uint64_t count = uint64_t{desc.extent.width} * desc.extent.height;
    if (desc.format == Format::D32Float) {
      depth_ = allocate_view<float>(allocator, count);
    } else if (desc.format != Format::Undefined) {
      color_ = allocate_view<uint32_t>(allocator, count);
    }
    for (float& d : depth_) d = 1.0f;
    for (uint32_t& c : color_) c = 0;
  }
  ~SoftwareImage() {
    release_view(allocator_, color_);
    release_view(allocator_, depth_);
  }
  bool valid() const { return !color_.empty() || !depth_.empty(); }
  Extent2D extent() const override { return desc_.extent; }
  Format format() const override { return desc_.format; }
  uint32_t usage() const { return desc_.usage; }
  bool read_pixels(ArrayView<uint32_t> out) override {
    if (color_.empty() || out.size() < color_.size()) return false;
    Surface s = surface();
    for (uint32_t i = 0; i < color_.size(); ++i) out[i] = s.decode(color_[i]);
    return true;
  }
  Surface surface() const {
    Surface s;
    s.pixels = color_;
    s.width = desc_.extent.width;
    s.height = desc_.extent.height;
    s.pitch = desc_.extent.width;
    s.format = desc_.format;
    return s;
  }
  ArrayView<float> depth() const { return depth_; }

 private:
  allocators::IAllocator& allocator_;
  ImageDesc desc_;
  ArrayView<uint32_t> color_;
  ArrayView<float> depth_;
};

class SoftwareShaderModule final : public IShaderModule {
 public:
  explicit SoftwareShaderModule(const ShaderModuleDesc& desc) : desc_(desc) {}
  ShaderStage stage() const override { return desc_.stage; }
  const ShaderModuleDesc& desc() const { return desc_; }

 private:
  ShaderModuleDesc desc_;
};

class SoftwarePipeline final : public IPipeline {
 public:
  SoftwarePipeline(const GraphicsPipelineDesc& desc, VertexShaderFn vs, FragmentShaderFn fs)
      : desc_(desc), vertex_(vs), fragment_(fs) {}
  const GraphicsPipelineDesc& desc() const { return desc_; }
  VertexShaderFn vertex() const { return vertex_; }
  FragmentShaderFn fragment() const { return fragment_; }

 private:
  GraphicsPipelineDesc desc_;
  VertexShaderFn vertex_;
  FragmentShaderFn fragment_;
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
  SoftwareImage* depth = nullptr;
  Viewport viewport;
  Rect2D scissor;  // already clamped to the target
  const uint8_t* constants = nullptr;
  uint8_t constants_storage[kMaxPushConstantBytes] = {};
};

float read_float(ConstByteView bytes, uint64_t offset) {
  float v = 0;
  const uint8_t* src = bytes.try_at(offset);
  if (src && offset + 4 <= bytes.size()) __builtin_memcpy(&v, src, 4);
  return v;
}

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

  const Surface target = s.color->surface();
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

  for (int32_t y = min_y; y < max_y; ++y) {
    for (int32_t x = min_x; x < max_x; ++x) {
      const float px = x + 0.5f, py = y + 0.5f;
      const float w0 = (px - v1->x) * e0dy - (py - v1->y) * e0dx;
      const float w1 = (px - v2->x) * e1dy - (py - v2->y) * e1dx;
      const float w2 = (px - v0->x) * e2dy - (py - v0->y) * e2dx;
      // With y down and a positive area, an interior point has all three edge values <= 0 in this
      // formulation, so flip the sign to get weights.
      const float l0 = -w0, l1 = -w1, l2 = -w2;
      if (l0 < 0 || l1 < 0 || l2 < 0) continue;
      if ((l0 == 0 && !own0) || (l1 == 0 && !own1) || (l2 == 0 && !own2)) continue;
      const float b0 = l0 * inv_area, b1 = l1 * inv_area, b2 = l2 * inv_area;

      const uint64_t index = uint64_t{static_cast<uint32_t>(y)} * s.color->extent().width + static_cast<uint32_t>(x);
      const float z = b0 * v0->z + b1 * v1->z + b2 * v2->z;
      if (pd.depth_test && !depth.empty()) {
        if (!(z <= depth[index])) continue;
      }

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
      Vec4 color;
      if (!s.pipeline->fragment()(varying, s.constants, &color)) continue;

      uint32_t out = pack_color(color);
      uint32_t* pixel = target.row(static_cast<uint32_t>(y)) + x;
      if (pd.alpha_blend) out = blend_over(out, target.decode(*pixel));
      *pixel = target.encode(out);
      if (pd.depth_write && !depth.empty()) depth[index] = z;
    }
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

ClipVertex run_vertex_shader(const SoftwarePipeline& pipeline, ConstByteView vertex_data, uint64_t byte_offset,
                             const uint8_t* constants) {
  const GraphicsPipelineDesc& pd = pipeline.desc();
  Vec4 attributes[kMaxVertexAttributes];
  for (Vec4& a : attributes) a = {0, 0, 0, 1};
  for (uint32_t i = 0; i < pd.attribute_count; ++i) {
    const VertexAttribute& at = pd.attributes[i];
    if (at.location >= kMaxVertexAttributes) continue;
    const uint32_t components = static_cast<uint32_t>(at.format) + 1;
    float v[4] = {0, 0, 0, 1};
    for (uint32_t k = 0; k < components; ++k) v[k] = read_float(vertex_data, byte_offset + at.offset + 4 * k);
    attributes[at.location] = {v[0], v[1], v[2], v[3]};
  }
  ShaderIo io;
  pipeline.vertex()(attributes, constants, &io);
  ClipVertex out;
  out.position = io.position;
  for (uint32_t i = 0; i < kMaxVaryings; ++i) out.varying[i] = io.varying[i];
  return out;
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
  SoftwareImage* image = make<SoftwareImage>(allocator_, allocator_, desc);
  if (image && !image->valid()) {
    destroy(allocator_, image);
    return nullptr;
  }
  return image;
}
void SoftwareDevice::destroy_image(IImage* image) { destroy(allocator_, static_cast<SoftwareImage*>(image)); }

IShaderModule* SoftwareDevice::create_shader_module(const ShaderModuleDesc& desc) {
  if (desc.stage == ShaderStage::Vertex ? !desc.vertex_function : !desc.fragment_function) return nullptr;
  return make<SoftwareShaderModule>(allocator_, desc);
}
void SoftwareDevice::destroy_shader_module(IShaderModule* module) {
  destroy(allocator_, static_cast<SoftwareShaderModule*>(module));
}

IPipeline* SoftwareDevice::create_graphics_pipeline(const GraphicsPipelineDesc& desc) {
  if (!desc.vertex_shader || !desc.fragment_shader || desc.varying_count > kMaxVaryings ||
      desc.attribute_count > kMaxVertexAttributes)
    return nullptr;
  const auto* vs = static_cast<const SoftwareShaderModule*>(desc.vertex_shader);
  const auto* fs = static_cast<const SoftwareShaderModule*>(desc.fragment_shader);
  if (vs->stage() != ShaderStage::Vertex || fs->stage() != ShaderStage::Fragment) return nullptr;
  return make<SoftwarePipeline>(allocator_, desc, vs->desc().vertex_function, fs->desc().fragment_function);
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
  const CommandRecorder& recorder = *static_cast<CommandRecorder*>(commands);
  DrawState s;
  IBuffer* vertex_buffer = nullptr;
  uint64_t vertex_offset = 0;
  IBuffer* index_buffer = nullptr;
  uint64_t index_offset = 0;
  IndexType index_type = IndexType::Uint16;
  bool in_pass = false;
  s.constants = s.constants_storage;
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
        const Extent2D e = s.color->extent();
        if (s.depth && (s.depth->extent().width != e.width || s.depth->extent().height != e.height)) return false;
        if (cmd.pass.clear_color) {
          const Surface surface = s.color->surface();
          const uint32_t v = surface.encode(pack_color(cmd.pass.clear_color_value));
          for (uint32_t y = 0; y < e.height; ++y)
            for (uint32_t x = 0; x < e.width; ++x) surface.row(y)[x] = v;
        }
        if (s.depth && cmd.pass.clear_depth)
          for (float& d : s.depth->depth()) d = cmd.pass.clear_depth_value;
        s.viewport = {0, 0, static_cast<float>(e.width), static_cast<float>(e.height), 0, 1};
        s.scissor = {0, 0, e.width, e.height};
        s.pipeline = nullptr;
        in_pass = true;
        break;
      }
      case RecordedCommand::Type::EndRenderPass:
        in_pass = false;
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
      case RecordedCommand::Type::SetViewport:
        s.viewport = cmd.viewport;
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
        const ConstByteView vertices = ConstByteView(vertex_buffer->map());
        const ConstByteView indices = indexed ? ConstByteView(index_buffer->map()) : ConstByteView();
        const uint32_t index_size = index_type == IndexType::Uint16 ? 2 : 4;
        auto vertex_at = [&](uint32_t n, ClipVertex* out) {
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
          *out = run_vertex_shader(*s.pipeline, vertices, byte, s.constants);
          return true;
        };
        for (uint32_t t = 0; t + 2 < cmd.count; t += 3) {
          ClipVertex tri[3];
          if (!vertex_at(t, &tri[0]) || !vertex_at(t + 1, &tri[1]) || !vertex_at(t + 2, &tri[2])) return false;
          draw_triangle(s, tri[0], tri[1], tri[2]);
        }
        break;
      }
    }
  }
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
