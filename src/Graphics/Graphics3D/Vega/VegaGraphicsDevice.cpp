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

#include "Graphics/Graphics3D/Vega/VegaGraphicsDevice.hpp"
#include <new>
#include "Hardware/GraphicsProcessingUnit/AMD/Gcn/GcnCompiler.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ByteStream.hpp"
#include "Utility/AllocatedView.hpp"

namespace graphics {

using hardware::AmdVegaDraw;
using hardware::VegaDrawRequest;
using hardware::VegaTexture;

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

// A view of `bytes` bytes that lives as long as the caller keeps it (grown when a draw needs more, never shrunk).
bool ensure(allocators::IAllocator& allocator, ByteView* view, uint64_t bytes) {
  if (view->size() >= bytes) return true;
  release_view(allocator, *view);
  *view = allocate_view<uint8_t>(allocator, bytes);
  return view->size() >= bytes;
}

void put_floats(ByteView memory, uint64_t offset, const float (&v)[4]) {
  ByteWriter w(memory.subview(offset, 16));
  for (float f : v) w.le32(__builtin_bit_cast(uint32_t, f));
}

float get_float(ConstByteView memory, uint64_t offset) {
  ByteReader r(memory.subview(offset, 4));
  return __builtin_bit_cast(float, r.le32());
}

// --- Resources ---------------------------------------------------------------

class VegaBuffer final : public IBuffer {
 public:
  VegaBuffer(allocators::IAllocator& allocator, uint64_t size) : allocator_(allocator) {
    data_ = allocate_view<uint8_t>(allocator, size);
    if (!data_.empty()) data_.fill(0);
  }
  ~VegaBuffer() { release_view(allocator_, data_); }
  bool valid() const { return !data_.empty(); }
  uint64_t size() const override { return data_.size(); }
  ByteView map() override { return data_; }
  void unmap() override {}

 private:
  allocators::IAllocator& allocator_;
  ByteView data_;
};

// A colour image (0xAARRGGBB, whatever the byte order of its format: the API's colours are always that), or a depth image (floats and optionally a stencil plane).
class VegaImage final : public IImage {
 public:
  VegaImage(allocators::IAllocator& allocator, const ImageDesc& desc) : allocator_(allocator), desc_(desc) {
    const uint64_t count = uint64_t{desc.extent.width} * desc.extent.height;
    if (desc.format == Format::D32Float) {
      depth_ = allocate_view<float>(allocator, count);
      if (desc.stencil) stencil_ = allocate_view<uint8_t>(allocator, count);
    } else {
      color_ = allocate_view<uint32_t>(allocator, count);
    }
    for (uint32_t& c : color_) c = 0;
    for (float& d : depth_) d = 1.0f;
    for (uint8_t& s : stencil_) s = 0;
  }
  ~VegaImage() {
    release_view(allocator_, color_);
    release_view(allocator_, depth_);
    release_view(allocator_, stencil_);
  }
  bool valid() const { return (!color_.empty() || !depth_.empty()) && (!desc_.stencil || !stencil_.empty()); }
  Extent2D extent() const override { return desc_.extent; }
  Format format() const override { return desc_.format; }
  uint32_t usage() const { return desc_.usage; }
  bool read_pixels(ArrayView<uint32_t> out) override {
    if (color_.empty() || out.size() < color_.size()) return false;
    return out.copy_from(ArrayView<const uint32_t>(color_.data(), color_.size()));
  }
  bool write_pixels(ArrayView<const uint32_t> pixels) override {
    if (color_.empty() || pixels.size() != color_.size()) return false;
    return color_.copy_from(pixels);
  }
  ArrayView<uint32_t> color() const { return color_; }
  ArrayView<float> depth() const { return depth_; }
  ArrayView<uint8_t> stencil() const { return stencil_; }

 private:
  allocators::IAllocator& allocator_;
  ImageDesc desc_;
  ArrayView<uint32_t> color_;
  ArrayView<float> depth_;
  ArrayView<uint8_t> stencil_;
};

class VegaModule final : public IShaderModule {
 public:
  VegaModule(allocators::IAllocator& allocator, ShaderStage stage) : stage_(stage), code_(allocator) {}
  ShaderStage stage() const override { return stage_; }
  const List<uint32_t>& code() const { return code_; }
  List<uint32_t>& code() { return code_; }
  gcn::ShaderInfo info;

 private:
  ShaderStage stage_;
  List<uint32_t> code_;
};

// --- State translation ----------------------------------------------------------

uint8_t blend_factor(BlendFactor f) {
  // (graphics order: zero, one, src colour, 1 - src colour, src alpha, 1 - src alpha, dst colour, 1 - dst colour, dst alpha, 1 - dst alpha, saturate; the hardware's: dst alpha is 6, dst colour 8)
  static const uint8_t kMap[11] = {0, 1, 2, 3, 4, 5, 8, 9, 6, 7, 10};
  return kMap[static_cast<uint32_t>(f)];
}

uint8_t blend_op(BlendOp op) {
  static const uint8_t kMap[5] = {0, 1, 4, 2, 3};   // add, subtract (src - dst), reverse subtract (dst - src), min, max
  return kMap[static_cast<uint32_t>(op)];
}

uint8_t stencil_op(StencilOp op) {
  static const uint8_t kMap[8] = {0, 1, 3, 5, 6, 7, 8, 9};   // keep, zero, replace (the reference), add / subtract clamped, invert, add / subtract wrapped
  return kMap[static_cast<uint32_t>(op)];
}

gcn::BlendState translate_blend(const GraphicsPipelineDesc& d) {
  gcn::BlendState b;
  if (d.blend.custom) {
    b.enable = d.blend.enable;
    b.color_src = blend_factor(d.blend.src_color);
    b.color_dst = blend_factor(d.blend.dst_color);
    b.color_op = blend_op(d.blend.color_op);
    b.separate_alpha = true;
    b.alpha_src = blend_factor(d.blend.src_alpha);
    b.alpha_dst = blend_factor(d.blend.dst_alpha);
    b.alpha_op = blend_op(d.blend.alpha_op);
    b.write_mask = d.blend.write_mask;
  } else if (d.alpha_blend) {
    b.enable = true;   // source over destination, the alpha too (src + dst * (1 - src))
    b.color_src = 4;
    b.color_dst = 5;
    b.separate_alpha = true;
    b.alpha_src = 1;
    b.alpha_dst = 5;
  }
  return b;
}

gcn::DepthStencilState translate_depth_stencil(const GraphicsPipelineDesc& d) {
  gcn::DepthStencilState s;
  s.depth_test = d.depth_test;
  s.depth_write = d.depth_write;
  s.depth_func = d.depth_test ? static_cast<uint8_t>(d.depth_compare) : 7;   // (depth_test off: every fragment passes, and only writes if asked)
  if (!d.depth_test && d.depth_write) s.depth_test = true;                    // (the plane is written by a draw that tests: always pass)
  if (d.stencil.enable) {
    s.stencil_test = true;
    s.stencil_func = static_cast<uint8_t>(d.stencil.compare);
    s.stencil_ref = d.stencil.reference;
    s.stencil_mask = d.stencil.compare_mask;
    s.stencil_write_mask = d.stencil.write_mask;
    s.fail_op = stencil_op(d.stencil.fail_op);
    s.zfail_op = stencil_op(d.stencil.depth_fail_op);
    s.zpass_op = stencil_op(d.stencil.pass_op);
  }
  s.front_clockwise = d.front_face == FrontFace::Clockwise;
  s.cull = d.cull_mode == CullMode::Back ? 2 : d.cull_mode == CullMode::Front ? 1 : 0;
  return s;
}

class VegaPipeline final : public IPipeline {
 public:
  VegaPipeline(allocators::IAllocator& allocator, const GraphicsPipelineDesc& desc) : desc_(desc), vertex_code_(allocator), pixel_code_(allocator) {}
  GraphicsPipelineDesc desc_;
  List<uint32_t> vertex_code_, pixel_code_;
  gcn::ShaderInfo vertex_info, pixel_info;
  gcn::BlendState blend;
  gcn::DepthStencilState depth_stencil;
};

}  // namespace

// --- The device ------------------------------------------------------------------

struct VegaGraphicsDevice::Scratch {
  explicit Scratch(allocators::IAllocator& a) : allocator(a), indices(a) {}
  ~Scratch() {
    release_view(allocator, area);
    release_view(allocator, target);
    release_view(allocator, depth);
    release_view(allocator, stencil);
    release_view(allocator, initial_target);
    release_view(allocator, initial_depth);
    release_view(allocator, initial_stencil);
    for (ByteView& v : texture) release_view(allocator, v);
  }
  allocators::IAllocator& allocator;
  ByteView area, target, depth, stencil, initial_target, initial_depth, initial_stencil, texture[kMaxTextureUnits];
  List<uint32_t> indices;
};

// Everything one draw needs from the command stream's state.
struct VegaGraphicsDevice::DrawArgs {
  const VegaPipeline* pipeline = nullptr;
  VegaImage* color = nullptr;
  VegaImage* depth = nullptr;
  Rect2D scissor;
  IBuffer* vertex_buffer = nullptr;
  uint64_t vertex_offset = 0;
  IBuffer* index_buffer = nullptr;
  uint64_t index_offset = 0;
  IndexType index_type = IndexType::Uint16;
  bool indexed = false;
  uint32_t count = 0, first = 0;
  int32_t base_vertex = 0;
  const float* constants = nullptr;   // rows of four
  uint32_t constant_rows = 0;
  VegaImage* textures[kMaxTextureUnits] = {};
  SamplerDesc samplers[kMaxTextureUnits];
};

VegaGraphicsDevice::VegaGraphicsDevice(allocators::IAllocator& allocator, hardware::AmdVegaM& card) : allocator_(allocator), card_(card) {
  usable_ = card.draw_area(&area_offset_, &area_bytes_);
  scratch_ = make<Scratch>(allocator, allocator);
  if (!scratch_) usable_ = false;
}

VegaGraphicsDevice::~VegaGraphicsDevice() {
  destroy(allocator_, scratch_);
}

IBuffer* VegaGraphicsDevice::create_buffer(uint64_t size, uint32_t) {
  VegaBuffer* b = make<VegaBuffer>(allocator_, allocator_, size);
  if (b && !b->valid() && size != 0) {
    destroy(allocator_, b);
    return nullptr;
  }
  return b;
}
void VegaGraphicsDevice::destroy_buffer(IBuffer* buffer) { destroy(allocator_, static_cast<VegaBuffer*>(buffer)); }

IImage* VegaGraphicsDevice::create_image(const ImageDesc& desc) {
  // (colour 8-bit images and depth images, one level, one layer, 2D: the rest is refused)
  if (desc.extent.width == 0 || desc.extent.height == 0 || desc.mip_levels != 1 || desc.kind != TextureKind::Texture2D) return nullptr;
  if (desc.format != Format::B8G8R8A8Unorm && desc.format != Format::R8G8B8A8Unorm && desc.format != Format::D32Float) return nullptr;
  if (desc.format == Format::D32Float ? (desc.usage & ~kImageDepthAttachment) != 0 : desc.stencil) return nullptr;
  VegaImage* image = make<VegaImage>(allocator_, allocator_, desc);
  if (image && !image->valid()) {
    destroy(allocator_, image);
    return nullptr;
  }
  return image;
}
void VegaGraphicsDevice::destroy_image(IImage* image) { destroy(allocator_, static_cast<VegaImage*>(image)); }

IShaderModule* VegaGraphicsDevice::create_shader_module(const ShaderModuleDesc& desc) {
  if (!desc.tgsi) return nullptr;
  gcn::TgsiShader shader(allocator_);
  const char* error = nullptr;
  uint32_t line = 0;
  if (!gcn::parse_tgsi(allocator_, StringView(desc.tgsi), &shader, &error, &line)) return nullptr;
  gcn::Code code(allocator_);
  gcn::ShaderInfo info;
  if (!gcn::compile_tgsi(shader, code, &info, &error) || !code.ok() || info.fragment != (desc.stage == ShaderStage::Fragment)) return nullptr;
  VegaModule* module = make<VegaModule>(allocator_, allocator_, desc.stage);
  if (!module) return nullptr;
  module->info = info;
  for (const uint32_t w : code.words())
    if (!module->code().push_back(w)) {
      destroy(allocator_, module);
      return nullptr;
    }
  return module;
}
void VegaGraphicsDevice::destroy_shader_module(IShaderModule* module) { destroy(allocator_, static_cast<VegaModule*>(module)); }

IPipeline* VegaGraphicsDevice::create_graphics_pipeline(const GraphicsPipelineDesc& desc) {
  const VegaModule* vs = static_cast<const VegaModule*>(desc.vertex_shader);
  const VegaModule* fs = static_cast<const VegaModule*>(desc.fragment_shader);
  if (!vs || !fs || vs->stage() != ShaderStage::Vertex || fs->stage() != ShaderStage::Fragment) return nullptr;
  if (desc.color_target_count != 1 || desc.independent_blend || desc.texture_count > kMaxTextureUnits || desc.attribute_count > kMaxVertexAttributes || desc.vertex_stride == 0 || desc.vertex_stride % 4 != 0)
    return nullptr;
  for (uint32_t i = 0; i < desc.attribute_count; ++i)
    if (desc.attributes[i].location >= gcn::abi::kMaxAttributes || desc.attributes[i].per_instance || desc.attributes[i].offset % 4 != 0) return nullptr;
  if (fs->info.color_outputs != 1 || (fs->info.samplers_used >> desc.texture_count) != 0) return nullptr;
  VegaPipeline* p = make<VegaPipeline>(allocator_, allocator_, desc);
  if (!p) return nullptr;
  p->desc_.vertex_shader = p->desc_.fragment_shader = nullptr;   // (the code is copied: the modules may go first)
  for (const uint32_t w : vs->code()) p->vertex_code_.push_back(w);
  for (const uint32_t w : fs->code()) p->pixel_code_.push_back(w);
  if (p->vertex_code_.count() != vs->code().count() || p->pixel_code_.count() != fs->code().count()) {
    destroy(allocator_, p);
    return nullptr;
  }
  p->vertex_info = vs->info;
  p->pixel_info = fs->info;
  p->blend = translate_blend(desc);
  p->depth_stencil = translate_depth_stencil(desc);
  return p;
}
void VegaGraphicsDevice::destroy_pipeline(IPipeline* pipeline) { destroy(allocator_, static_cast<VegaPipeline*>(pipeline)); }

ICommandBuffer* VegaGraphicsDevice::create_command_buffer() { return make<CommandRecorder>(allocator_, allocator_); }
void VegaGraphicsDevice::destroy_command_buffer(ICommandBuffer* commands) { destroy(allocator_, static_cast<CommandRecorder*>(commands)); }

// One draw on the card: the images' contents as the starting point, the planes that come back merged inside the scissor.
bool VegaGraphicsDevice::run_draw(const DrawArgs& a) {
  const VegaPipeline& pipe = *a.pipeline;
  const GraphicsPipelineDesc& pd = pipe.desc_;
  Scratch& s = *scratch_;
  if (a.count == 0 || a.count % 3 != 0) return fail("a draw that is not a list of triangles");
  const uint32_t width = a.color->extent().width, height = a.color->extent().height;

  VegaDrawRequest r;
  r.vertex_code = ArrayView<const uint32_t>(pipe.vertex_code_.view().data(), pipe.vertex_code_.count());
  r.pixel_code = ArrayView<const uint32_t>(pipe.pixel_code_.view().data(), pipe.pixel_code_.count());
  r.vertex_info = pipe.vertex_info;
  r.pixel_info = pipe.pixel_info;
  r.width = width;
  r.height = height;
  r.device_clip_space = true;
  r.blend = pipe.blend;
  r.depth_stencil = pipe.depth_stencil;
  if (!a.depth) {   // (nothing to test against: every fragment passes, as on the software device)
    r.depth_stencil.depth_test = r.depth_stencil.depth_write = false;
    if (pd.stencil.enable) return fail("a stencil test without a depth image");
  } else if (pd.stencil.enable && a.depth->stencil().empty()) {
    return fail("a stencil test without a stencil plane");
  }
  for (uint32_t i = 0; i < pd.attribute_count; ++i) {
    const VertexAttribute& at = pd.attributes[i];
    r.attributes[at.location] = {static_cast<uint32_t>(at.format) + 1, at.offset};
  }
  r.vertex_stride = pd.vertex_stride;
  r.ps_param[0] = 0;   // (the interpolants in order: the default)
  // the constants: the same rows for both stages
  r.vertex_constants = ArrayView<const float>(a.constants, uint64_t{a.constant_rows} * 4);
  r.pixel_constants = r.vertex_constants;

  // the vertices, and the indices as 32 bit values with the base vertex added
  const ByteView vertices = a.vertex_buffer->map();
  if (a.vertex_offset > vertices.size()) return fail("the vertex buffer offset is outside the buffer");
  ByteView region;
  if (!vertices.try_subview(a.vertex_offset, vertices.size() - a.vertex_offset, &region)) return fail("the vertex buffer offset is outside the buffer");
  const uint64_t stride = pd.vertex_stride;
  uint64_t first_vertex_byte = 0;
  if (a.indexed) {
    const ByteView index_memory = a.index_buffer->map();
    const uint32_t index_size = a.index_type == IndexType::Uint16 ? 2 : 4;
    s.indices.clear();
    for (uint32_t n = 0; n < a.count; ++n) {
      const uint64_t at = a.index_offset + uint64_t{a.first + n} * index_size;
      if (at > index_memory.size() || index_memory.size() - at < index_size) return fail("an index beyond the index buffer");
      uint32_t v = index_memory[at] | (uint32_t{index_memory[at + 1]} << 8);
      if (index_size == 4) v |= (uint32_t{index_memory[at + 2]} << 16) | (uint32_t{index_memory[at + 3]} << 24);
      const int64_t with_base = int64_t{v} + a.base_vertex;
      if (with_base < 0 || (static_cast<uint64_t>(with_base) + 1) * stride > region.size()) return fail("an index beyond the vertex buffer");
      if (!s.indices.push_back(static_cast<uint32_t>(with_base))) return fail("out of memory");
    }
    r.indices = ArrayView<const uint32_t>(s.indices.view().data(), s.indices.count());
  } else {
    first_vertex_byte = uint64_t{a.first} * stride;
    if (first_vertex_byte > region.size() || uint64_t{a.count} * stride > region.size() - first_vertex_byte) return fail("a draw beyond the vertex buffer");
    r.vertex_count = a.count;
  }
  ByteView vertex_bytes = region.subview(first_vertex_byte, region.size() - first_vertex_byte);
  vertex_bytes = vertex_bytes.first(vertex_bytes.size() / 4 * 4);
  ArrayView<uint32_t> vertex_words;
  if (!vertex_bytes.try_cast<uint32_t>(&vertex_words)) return fail("the vertex buffer is not aligned to words");
  r.vertex_data = ArrayView<const uint32_t>(vertex_words.data(), vertex_words.size());

  // the textures: 2D RGBA8, the rows as they are
  for (uint32_t u = 0; u < pd.texture_count; ++u) {
    const VegaImage* t = a.textures[u];
    if (!t || t->color().empty() || !(t->usage() & kImageSampled)) return fail("a texture that is not bound");
    const uint64_t count = t->color().size();
    if (!ensure(allocator_, &s.texture[u], count * 4)) return fail("out of memory");
    ByteWriter w(s.texture[u].first(count * 4));
    for (uint32_t argb : t->color()) w.le32(((argb >> 16) & 0xFF) | (argb & 0xFF00) | ((argb & 0xFF) << 16) | (argb & 0xFF000000u));   // (RGBA in memory order)
    VegaTexture& vt = r.textures[u];
    vt.format = gcn::TexelFormat::Rgba8Unorm;
    vt.width = t->extent().width;
    vt.height = t->extent().height;
    vt.tile_index = 8;
    vt.texels = ConstByteView(s.texture[u].data(), count * 4);
    vt.sampler.bilinear = a.samplers[u].filter == Filter::Linear;
    vt.sampler.clamp_x = a.samplers[u].address_u == AddressMode::Repeat ? 0 : 2;
    vt.sampler.clamp_y = a.samplers[u].address_v == AddressMode::Repeat ? 0 : 2;
  }

  // the planes as the draw starts
  const uint64_t target_bytes = AmdVegaDraw::target_bytes(r), depth_bytes = AmdVegaDraw::depth_bytes(r), stencil_bytes = AmdVegaDraw::stencil_bytes(r), area = AmdVegaDraw::area_bytes(r);
  if (target_bytes == 0 || area == 0 || area > area_bytes_ || area_offset_ % AmdVegaDraw::area_alignment(r) != 0) return fail("the draw does not fit the card's draw area");
  if (!ensure(allocator_, &s.area, area) || !ensure(allocator_, &s.target, target_bytes) || !ensure(allocator_, &s.initial_target, target_bytes)) return fail("out of memory");
  if (depth_bytes && (!ensure(allocator_, &s.depth, depth_bytes) || !ensure(allocator_, &s.initial_depth, depth_bytes))) return fail("out of memory");
  if (stencil_bytes && (!ensure(allocator_, &s.stencil, stencil_bytes) || !ensure(allocator_, &s.initial_stencil, stencil_bytes))) return fail("out of memory");
  s.initial_target.first(target_bytes).fill(0);
  if (depth_bytes) s.initial_depth.first(depth_bytes).fill(0);
  if (stencil_bytes) s.initial_stencil.first(stencil_bytes).fill(0);
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      const Vec4 c = unpack_color(a.color->color()[uint64_t{y} * width + x]);
      put_floats(s.initial_target, AmdVegaDraw::pixel_offset(r, x, y), {c.x, c.y, c.z, c.w});
      if (depth_bytes) {
        ByteWriter w(s.initial_depth.subview(AmdVegaDraw::depth_offset(r, x, y), 4));
        w.le32(__builtin_bit_cast(uint32_t, a.depth->depth()[uint64_t{y} * width + x]));
      }
      if (stencil_bytes) s.initial_stencil[AmdVegaDraw::stencil_offset(r, x, y)] = a.depth->stencil()[uint64_t{y} * width + x];
    }
  }
  r.initial_target = ConstByteView(s.initial_target.data(), target_bytes);
  if (depth_bytes) r.initial_depth = ConstByteView(s.initial_depth.data(), depth_bytes);
  if (stencil_bytes) r.initial_stencil = ConstByteView(s.initial_stencil.data(), stencil_bytes);

  if (!AmdVegaDraw::draw(card_, r, area_offset_, s.area.first(area), s.target.first(target_bytes), depth_bytes ? s.depth.first(depth_bytes) : ByteView(), stencil_bytes ? s.stencil.first(stencil_bytes) : ByteView()))
    return fail("the card did not run the draw");
  ++draws_;

  // merge what came back, inside the scissor
  const int64_t x0 = a.scissor.x, y0 = a.scissor.y, x1 = x0 + a.scissor.width, y1 = y0 + a.scissor.height;
  for (int64_t y = y0 < 0 ? 0 : y0; y < y1 && y < height; ++y) {
    for (int64_t x = x0 < 0 ? 0 : x0; x < x1 && x < width; ++x) {
      const uint32_t px = static_cast<uint32_t>(x), py = static_cast<uint32_t>(y);
      const uint64_t at = uint64_t{py} * width + px;
      const uint64_t o = AmdVegaDraw::pixel_offset(r, px, py);
      a.color->color()[at] = pack_color({get_float(s.target, o), get_float(s.target, o + 4), get_float(s.target, o + 8), get_float(s.target, o + 12)});
      if (depth_bytes) a.depth->depth()[at] = get_float(s.depth, AmdVegaDraw::depth_offset(r, px, py));
      if (stencil_bytes) a.depth->stencil()[at] = s.stencil[AmdVegaDraw::stencil_offset(r, px, py)];
    }
  }
  return true;
}

bool VegaGraphicsDevice::submit(ICommandBuffer* commands) {
  if (!commands) return false;
  if (!usable_) return fail("the card has no room for draws");
  error_ = "";
  CommandRecorder& recorder = *static_cast<CommandRecorder*>(commands);
  DrawArgs a;
  float constants[kMaxPushConstantBytes / 4] = {};
  uint32_t constant_rows = 0;
  bool in_pass = false;
  PrimitiveTopology topology = PrimitiveTopology::TriangleList;
  Viewport viewport;
  const ConstByteView pool = recorder.constants();

  for (uint64_t i = 0; i < recorder.commands().count(); ++i) {
    const RecordedCommand& cmd = recorder.commands()[i];
    switch (cmd.type) {
      case RecordedCommand::Type::BeginRenderPass: {
        VegaImage* color = static_cast<VegaImage*>(cmd.pass.color);
        VegaImage* depth = static_cast<VegaImage*>(cmd.pass.depth);
        if (in_pass || !color || cmd.pass.extra_colors[0]) return fail("a render pass this device cannot run");
        if (!(color->usage() & kImageColorAttachment) || color->color().empty()) return fail("a render target that is not a colour attachment");
        if (depth && (depth->depth().empty() || depth->extent().width != color->extent().width || depth->extent().height != color->extent().height)) return fail("a depth image that does not match the target");
        if (cmd.pass.clear_color) color->color().fill(pack_color(cmd.pass.clear_color_value));
        if (depth && cmd.pass.clear_depth) depth->depth().fill(cmd.pass.clear_depth_value);
        if (depth && cmd.pass.clear_stencil && !depth->stencil().empty()) depth->stencil().fill(cmd.pass.clear_stencil_value);
        a.color = color;
        a.depth = depth;
        a.pipeline = nullptr;
        a.scissor = {0, 0, color->extent().width, color->extent().height};
        viewport = {0, 0, static_cast<float>(color->extent().width), static_cast<float>(color->extent().height), 0, 1};
        in_pass = true;
        break;
      }
      case RecordedCommand::Type::EndRenderPass:
        in_pass = false;
        break;
      case RecordedCommand::Type::BindPipeline:
        a.pipeline = static_cast<const VegaPipeline*>(cmd.pipeline);
        break;
      case RecordedCommand::Type::BindVertexBuffer:
        a.vertex_buffer = cmd.buffer;
        a.vertex_offset = cmd.offset;
        break;
      case RecordedCommand::Type::BindIndexBuffer:
        a.index_buffer = cmd.buffer;
        a.index_offset = cmd.offset;
        a.index_type = cmd.index_type;
        break;
      case RecordedCommand::Type::PushConstants: {
        ConstByteView block;
        if (!pool.try_subview(cmd.constants_offset, cmd.constants_size, &block) || block.size() % 16 != 0) return fail("bad push constants");
        ByteReader in(block);
        for (uint64_t k = 0; k < block.size() / 4; ++k) constants[k] = __builtin_bit_cast(float, in.le32());
        constant_rows = static_cast<uint32_t>(block.size() / 16);
        break;
      }
      case RecordedCommand::Type::BindTexture: {
        VegaImage* image = static_cast<VegaImage*>(cmd.image);
        if (cmd.unit >= kMaxTextureUnits || !image) return fail("a bad texture binding");
        a.textures[cmd.unit] = image;
        a.samplers[cmd.unit] = cmd.sampler;
        break;
      }
      case RecordedCommand::Type::SetViewport:
        viewport = cmd.viewport;
        break;
      case RecordedCommand::Type::SetScissor: {
        if (!in_pass) return fail("a scissor outside a pass");
        const Extent2D e = a.color->extent();
        const int64_t x0 = cmd.scissor.x > 0 ? cmd.scissor.x : 0, y0 = cmd.scissor.y > 0 ? cmd.scissor.y : 0;
        int64_t x1 = int64_t{cmd.scissor.x} + cmd.scissor.width, y1 = int64_t{cmd.scissor.y} + cmd.scissor.height;
        if (x1 > e.width) x1 = e.width;
        if (y1 > e.height) y1 = e.height;
        a.scissor = {static_cast<int32_t>(x0), static_cast<int32_t>(y0), x1 > x0 ? static_cast<uint32_t>(x1 - x0) : 0, y1 > y0 ? static_cast<uint32_t>(y1 - y0) : 0};
        break;
      }
      case RecordedCommand::Type::SetTopology:
        topology = cmd.topology;
        break;
      case RecordedCommand::Type::BeginQuery:
      case RecordedCommand::Type::EndQuery:
        return fail("occlusion queries");
      case RecordedCommand::Type::Draw:
      case RecordedCommand::Type::DrawIndexed: {
        if (!in_pass || !a.pipeline || !a.vertex_buffer) return fail("a draw without a pass, a pipeline or vertices");
        const bool indexed = cmd.type == RecordedCommand::Type::DrawIndexed;
        if (indexed && !a.index_buffer) return fail("an indexed draw without indices");
        if (topology != PrimitiveTopology::TriangleList) return fail("lines and points");
        const Extent2D e = a.color->extent();
        if (viewport.x != 0 || viewport.y != 0 || viewport.width != static_cast<float>(e.width) || viewport.height != static_cast<float>(e.height) || viewport.min_depth != 0 || viewport.max_depth != 1)
          return fail("a viewport other than the whole target");
        a.indexed = indexed;
        a.count = cmd.count;
        a.first = cmd.first;
        a.base_vertex = cmd.vertex_offset;
        a.constants = constants;
        a.constant_rows = constant_rows;
        if (a.scissor.width == 0 || a.scissor.height == 0) break;
        if (!run_draw(a)) return false;
        break;
      }
    }
  }
  return !in_pass;
}

}  // namespace graphics
