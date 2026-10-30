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

#include "Graphics/Graphics3D/V3d/V3dGraphicsDevice.hpp"
#include "Graphics/Graphics3D/BuiltinShaders.hpp"
#include <new>
#include "Hardware/GraphicsProcessingUnit/Broadcom/V3dDraw.hpp"
#include "Hardware/GraphicsProcessingUnit/Broadcom/V3dFloat.hpp"
#include "Hardware/GraphicsProcessingUnit/Broadcom/V3dRenderer.hpp"

namespace graphics {

using namespace hardware;

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

constexpr uint32_t kRowAlignmentPixels = 16;  // image rows start at a multiple of 64 bytes
constexpr uint32_t kOverflowBytes = 256 * 1024;
constexpr uint32_t kMaxPushedConstantBytes = 128;
constexpr uint32_t kMvpBytes = 64;
constexpr uint32_t kMaxDimension = 4096;

uint32_t align_up(uint32_t value, uint32_t alignment) { return (value + alignment - 1) & ~(alignment - 1); }

// --- Resources ---------------------------------------------------------------

class V3dBuffer final : public IBuffer {
 public:
  V3dBuffer(IV3dGpuMemory& memory, uint64_t size) : memory_(memory), size_(size) {
    if (size > 0 && size <= UINT32_MAX - 4096) {
      if (!memory.allocate(static_cast<uint32_t>(size), 4096, &block_)) block_ = V3dGpuBlock();
    }
  }
  ~V3dBuffer() { if (block_.valid()) memory_.release(block_); }
  bool valid() const { return block_.valid(); }
  uint64_t size() const override { return size_; }
  ByteView map() override { return block_.cpu.first(size_); }
  void unmap() override { memory_.clean(block_); }
  const V3dGpuBlock& block() const { return block_; }

 private:
  IV3dGpuMemory& memory_;
  uint64_t size_;
  V3dGpuBlock block_;
};

class V3dImage final : public IImage {
 public:
  V3dImage(IV3dGpuMemory& memory, const ImageDesc& desc) : memory_(memory), desc_(desc) {
    const uint32_t w = desc.extent.width, h = desc.extent.height;
    if (w == 0 || h == 0 || w > kMaxDimension || h > kMaxDimension || desc.format == Format::Undefined) return;
    pitch_ = align_up(w, kRowAlignmentPixels);
    if (!memory.allocate(pitch_ * h * 4, 4096, &block_)) {
      block_ = V3dGpuBlock();
      return;
    }
    if (desc.format == Format::D32Float) {
      ArrayView<uint32_t> words;
      if (block_.cpu.try_cast<uint32_t>(&words))
        for (uint32_t& word : words) word = 0x3F800000;  // 1.0f: nothing in front
    }
    memory.clean(block_);
  }
  ~V3dImage() { if (block_.valid()) memory_.release(block_); }
  bool valid() const { return block_.valid(); }
  Extent2D extent() const override { return desc_.extent; }
  Format format() const override { return desc_.format; }
  uint32_t usage() const { return desc_.usage; }
  uint32_t pitch_pixels() const { return pitch_; }
  uint32_t stride_bytes() const { return pitch_ * 4; }
  const V3dGpuBlock& block() const { return block_; }
  Surface surface() const {
    Surface s;
    ArrayView<uint32_t> words;
    if (desc_.format != Format::D32Float && block_.cpu.try_cast<uint32_t>(&words)) s.pixels = words;
    s.width = desc_.extent.width;
    s.height = desc_.extent.height;
    s.pitch = pitch_;
    s.format = desc_.format;
    return s;
  }
  bool read_pixels(ArrayView<uint32_t> out) override {
    const uint64_t count = uint64_t{desc_.extent.width} * desc_.extent.height;
    if (desc_.format == Format::D32Float || !valid() || out.size() < count) return false;
    memory_.invalidate(block_);
    const Surface s = surface();
    if (!s.valid()) return false;
    for (uint32_t y = 0; y < s.height; ++y)
      for (uint32_t x = 0; x < s.width; ++x) out[uint64_t{y} * s.width + x] = s.decode(s.row(y)[x]);
    return true;
  }

 private:
  IV3dGpuMemory& memory_;
  ImageDesc desc_;
  uint32_t pitch_ = 0;
  V3dGpuBlock block_;
};

class V3dShaderModule final : public IShaderModule {
 public:
  explicit V3dShaderModule(const ShaderModuleDesc& desc) : desc_(desc) {}
  ShaderStage stage() const override { return desc_.stage; }
  QpuProgram program() const { return desc_.qpu_program; }
  bool matrix_rows() const { return desc_.qpu_matrix_rows; }
  bool flip_y() const { return desc_.qpu_flip_y; }

 private:
  ShaderModuleDesc desc_;
};

class V3dPipeline final : public IPipeline {
 public:
  V3dPipeline(const GraphicsPipelineDesc& desc, uint32_t position_offset, uint32_t color_offset, bool matrix_rows = false, bool flip_y = false)
      : desc_(desc), position_offset_(position_offset), color_offset_(color_offset), matrix_rows_(matrix_rows), flip_y_(flip_y) {}
  bool matrix_rows() const { return matrix_rows_; }
  bool flip_y() const { return flip_y_; }
  const GraphicsPipelineDesc& desc() const { return desc_; }
  uint32_t position_offset() const { return position_offset_; }
  uint32_t color_offset() const { return color_offset_; }

 private:
  GraphicsPipelineDesc desc_;
  uint32_t position_offset_, color_offset_;
  bool matrix_rows_, flip_y_;
};

uint32_t word_from_bytes(const uint8_t* b) { return static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) | (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24); }

// A Vulkan viewport rounded to whole pixels (the V3D code takes integers); false if it is negative, not a number or does not fit.
bool round_viewport(const Viewport& in, V3dViewport* out) {
  const float limits[4] = {in.x, in.y, in.width, in.height};
  for (float v : limits)
    if (!(v >= 0.0f && v <= 16000.0f)) return false;
  out->x = static_cast<uint32_t>(in.x + 0.5f);
  out->y = static_cast<uint32_t>(in.y + 0.5f);
  out->width = static_cast<uint32_t>(in.width + 0.5f);
  out->height = static_cast<uint32_t>(in.height + 0.5f);
  if (!(in.min_depth >= 0.0f && in.max_depth <= 1.0f && in.min_depth <= in.max_depth)) return false;
  out->z_scale_bits = v3d_float_bits(in.max_depth - in.min_depth);
  out->z_offset_bits = v3d_float_bits(in.min_depth);
  out->z_max_bits = v3d_float_bits(in.max_depth);
  return true;
}

}  // namespace

// One draw of a render pass as recorded: what was bound and set when it was issued.
struct V3dGraphicsDevice::DrawRecord {
  const V3dPipeline* pipeline = nullptr;
  V3dBuffer* vertex_buffer = nullptr;
  uint64_t vertex_offset = 0;
  V3dBuffer* index_buffer = nullptr;
  uint64_t index_offset = 0;
  IndexType index_type = IndexType::Uint16;
  uint8_t constants[kMaxPushedConstantBytes] = {};
  uint32_t constants_size = 0;
  Viewport viewport;
  bool has_scissor = false;
  Rect2D scissor;
  bool indexed = false;
  uint32_t count = 0, first = 0;
  int32_t vertex_adjust = 0;
};

// Fills the binner's list of one pass with the state and the draws.
class V3dGraphicsDevice::PassContent final : public IV3dBinningContent {
 public:
  PassContent(const List<DrawRecord>& draws, const V3dFrameDesc& frame, const V3dGpuBlock& code, const V3dGpuBlock& defaults, const v3d_shader::ShaderCode& shaders)
      : draws_(draws), frame_(frame), code_(code), defaults_(defaults), shaders_(shaders) {}

  bool emit(V3dControlList& bcl, V3dControlList& indirect) override {
    for (uint64_t i = 0; i < draws_.count(); ++i) {
      const DrawRecord& d = draws_[i];
      if (!emit_draw(bcl, indirect, d)) return false;
    }
    return true;
  }

 private:
  bool emit_draw(V3dControlList& bcl, V3dControlList& indirect, const DrawRecord& d) const {
    if (d.constants_size < kMvpBytes || !d.vertex_buffer) return false;
    // The viewport defaults to the whole image and has to lie inside it.
    V3dViewport vp;
    if (d.viewport.width == 0 && d.viewport.height == 0) {
      vp.width = frame_.width;
      vp.height = frame_.height;
    } else if (!round_viewport(d.viewport, &vp)) {
      return false;
    }
    V3dClipRect clip;
    if (d.has_scissor) {
      const int64_t x0 = d.scissor.x < 0 ? 0 : d.scissor.x, y0 = d.scissor.y < 0 ? 0 : d.scissor.y;
      const int64_t x1 = d.scissor.x + int64_t{d.scissor.width}, y1 = d.scissor.y + int64_t{d.scissor.height};
      const int64_t cx1 = x1 > frame_.width ? frame_.width : x1, cy1 = y1 > frame_.height ? frame_.height : y1;
      if (cx1 <= x0 || cy1 <= y0) return true;  // nothing of the draw is visible
      clip = {static_cast<uint32_t>(x0), static_cast<uint32_t>(y0), static_cast<uint32_t>(cx1 - x0), static_cast<uint32_t>(cy1 - y0)};
    }
    const GraphicsPipelineDesc& pd = d.pipeline->desc();
    V3dDrawState state;
    state.cull = pd.cull_mode == CullMode::None ? V3dDrawState::Cull::None : pd.cull_mode == CullMode::Back ? V3dDrawState::Cull::Back : V3dDrawState::Cull::Front;
    state.front_clockwise = pd.front_face == FrontFace::Clockwise;
    state.depth_test = pd.depth_test;
    state.depth_write = pd.depth_write;
    state.alpha_blend = pd.alpha_blend;

    const uint32_t vertex_count = d.count - d.count % 3;  // triangles only
    if (vertex_count == 0) return true;

    // Which vertices the draw reaches, checked against the vertex buffer (the GPU would read whatever lies there).
    const uint64_t attribute_bytes = d.pipeline->color_offset() + 16 > d.pipeline->position_offset() + 12 ? d.pipeline->color_offset() + 16 : d.pipeline->position_offset() + 12;
    int64_t lowest = 0, highest = 0;
    V3dDrawCall call;
    call.code_address = code_.gpu_address;
    call.code = shaders_;
    call.with_z_write = frame_.has_depth;
    call.defaults_address = defaults_.gpu_address;
    if (d.indexed) {
      if (!d.index_buffer) return false;
      const uint32_t index_size = d.index_type == IndexType::Uint16 ? 2 : 4;
      const ByteView indices = d.index_buffer->map();
      if (d.index_offset % index_size != 0) return false;
      lowest = INT64_MAX;
      highest = INT64_MIN;
      for (uint32_t n = 0; n < vertex_count; ++n) {
        const uint64_t at = d.index_offset + uint64_t{d.first + n} * index_size;
        if (at > indices.size() || index_size > indices.size() - at) return false;
        int64_t v = indices[at] | (indices[at + 1] << 8);
        if (index_size == 4) v |= (static_cast<int64_t>(indices[at + 2]) << 16) | (static_cast<int64_t>(indices[at + 3]) << 24);
        v += d.vertex_adjust;
        if (v < lowest) lowest = v;
        if (v > highest) highest = v;
      }
      if (lowest < 0) return false;
      call.indexed = true;
      call.index_address = d.index_buffer->block().gpu_address;
      call.index_buffer_bytes = static_cast<uint32_t>(d.index_buffer->size());
      call.index_bytes = index_size;
      call.index_offset_bytes = static_cast<uint32_t>(d.index_offset + uint64_t{d.first} * index_size);
      call.base_vertex = d.vertex_adjust;
    } else {
      highest = int64_t{d.first} + vertex_count - 1;
      call.first_vertex = d.first;
    }
    const uint64_t last_byte = d.vertex_offset + static_cast<uint64_t>(highest) * pd.vertex_stride + attribute_bytes;
    if (highest < 0 || last_byte > d.vertex_buffer->size() || d.vertex_offset > UINT32_MAX) return false;

    call.vertex_count = vertex_count;
    const uint32_t base = d.vertex_buffer->block().gpu_address + static_cast<uint32_t>(d.vertex_offset);
    call.attribute_count = 2;
    call.attributes[0] = {base + d.pipeline->position_offset(), pd.vertex_stride, 3, 3, 3};
    call.attributes[1] = {base + d.pipeline->color_offset(), pd.vertex_stride, 4, 0, 4};

    // Uniform streams from the push constants (the matrix) and the viewport.
    uint32_t mvp[16];
    for (uint32_t k = 0; k < 16; ++k) mvp[k] = word_from_bytes(d.constants + 4 * k);
    kernel_matrix_words(mvp, d.pipeline->matrix_rows(), d.pipeline->flip_y());   // (a Direct3D 9 vertex shader: rows, Y up)
    uint32_t cs[v3d_shader::kCoordinateUniformWords], vs[v3d_shader::kVertexUniformWords], fs[v3d_shader::kFragmentZUniformWords];
    const uint32_t x_scale = v3d_shader::float_bits(vp.width * 128), y_scale = v3d_shader::float_bits(vp.height * 128);
    if (!v3d_shader::coordinate_uniforms(ArrayView<uint32_t>(cs), ArrayView<const uint32_t>(mvp), x_scale, y_scale) ||
        !v3d_shader::vertex_uniforms(ArrayView<uint32_t>(vs), ArrayView<const uint32_t>(mvp), x_scale, y_scale, vp.z_scale_bits, vp.z_offset_bits))
      return false;
    uint32_t fragment_words = 0;
    if (frame_.has_depth) fs[fragment_words++] = v3d_shader::kTlbDepthConfig;
    fs[fragment_words++] = v3d_shader::kTlbColorConfig;
    call.coordinate_uniforms = ArrayView<const uint32_t>(cs);
    call.vertex_uniforms = ArrayView<const uint32_t>(vs);
    call.fragment_uniforms = ArrayView<const uint32_t>(fs, fragment_words);

    return V3dDrawEmitter::emit_state(bcl, frame_, vp, state, d.has_scissor ? &clip : nullptr) && V3dDrawEmitter::emit_draw(bcl, indirect, call);
  }

  const List<DrawRecord>& draws_;
  const V3dFrameDesc& frame_;
  const V3dGpuBlock& code_;
  const V3dGpuBlock& defaults_;
  const v3d_shader::ShaderCode& shaders_;
};

V3dGraphicsDevice::~V3dGraphicsDevice() {
  if (code_.valid()) memory_.release(code_);
  if (defaults_.valid()) memory_.release(defaults_);
}

bool V3dGraphicsDevice::initialize() {
  if (ready()) return true;
  if (!gpu_.initialized()) return false;
  uint64_t words[256];
  ArrayView<uint64_t> storage(words, 256);
  if (!v3d_shader::build_colored_pipeline(storage, &shader_code_)) return false;
  const uint32_t count = shader_code_.fragment_z.first + shader_code_.fragment_z.instructions;
  if (!memory_.allocate(count * 8, 4096, &code_)) return false;
  for (uint32_t i = 0; i < count; ++i)
    for (uint32_t b = 0; b < 8; ++b) code_.cpu[i * 8 + b] = static_cast<uint8_t>(words[i] >> (8 * b));
  memory_.clean(code_);
  if (!memory_.allocate(256, 4096, &defaults_)) {
    memory_.release(code_);
    code_ = V3dGpuBlock();
    return false;
  }
  uint32_t defaults[64];
  V3dDrawEmitter::fill_attribute_defaults(ArrayView<uint32_t>(defaults));
  for (uint32_t i = 0; i < 64; ++i)
    for (uint32_t b = 0; b < 4; ++b) defaults_.cpu[i * 4 + b] = static_cast<uint8_t>(defaults[i] >> (8 * b));
  memory_.clean(defaults_);
  return true;
}

IBuffer* V3dGraphicsDevice::create_buffer(uint64_t size, uint32_t usage) {
  (void)usage;
  V3dBuffer* buffer = make<V3dBuffer>(allocator_, memory_, size);
  if (buffer && !buffer->valid()) {
    destroy(allocator_, buffer);
    return nullptr;
  }
  return buffer;
}
void V3dGraphicsDevice::destroy_buffer(IBuffer* buffer) { destroy(allocator_, static_cast<V3dBuffer*>(buffer)); }

IImage* V3dGraphicsDevice::create_image(const ImageDesc& desc) {
  if (desc.stencil) return nullptr;   // (no stencil plane on this device)
  if (desc.mip_levels != 1) return nullptr;   // (no mipmaps either)
  V3dImage* image = make<V3dImage>(allocator_, memory_, desc);
  if (image && !image->valid()) {
    destroy(allocator_, image);
    return nullptr;
  }
  return image;
}
void V3dGraphicsDevice::destroy_image(IImage* image) { destroy(allocator_, static_cast<V3dImage*>(image)); }

IShaderModule* V3dGraphicsDevice::create_shader_module(const ShaderModuleDesc& desc) {
  const bool matches = (desc.stage == ShaderStage::Vertex && desc.qpu_program == QpuProgram::ColoredVertex) ||
                       (desc.stage == ShaderStage::Fragment && desc.qpu_program == QpuProgram::ColoredFragment);
  return matches ? make<V3dShaderModule>(allocator_, desc) : nullptr;
}
void V3dGraphicsDevice::destroy_shader_module(IShaderModule* module) { destroy(allocator_, static_cast<V3dShaderModule*>(module)); }

IPipeline* V3dGraphicsDevice::create_graphics_pipeline(const GraphicsPipelineDesc& desc) {
  if (desc.texture_count != 0 || desc.fragment_shader == nullptr) return nullptr;   // (no textures on this device)
  if (desc.stencil.enable) return nullptr;   // (no stencil test on this device)
  const V3dShaderModule* vs = static_cast<const V3dShaderModule*>(desc.vertex_shader);
  const V3dShaderModule* fs = static_cast<const V3dShaderModule*>(desc.fragment_shader);
  if (!vs || !fs || vs->program() != QpuProgram::ColoredVertex || fs->program() != QpuProgram::ColoredFragment) return nullptr;
  if (desc.attribute_count != 2 || desc.varying_count != 1 || desc.vertex_stride == 0 || desc.vertex_stride > 2048) return nullptr;
  int32_t position = -1, color = -1;
  for (uint32_t i = 0; i < desc.attribute_count; ++i) {
    const VertexAttribute& a = desc.attributes[i];
    if (a.location == 0 && a.format == VertexFormat::Float3) position = static_cast<int32_t>(a.offset);
    else if (a.location == 1 && a.format == VertexFormat::Float4) color = static_cast<int32_t>(a.offset);
  }
  if (position < 0 || color < 0 || position > 2000 || color > 2000) return nullptr;
  return make<V3dPipeline>(allocator_, desc, static_cast<uint32_t>(position), static_cast<uint32_t>(color), vs->matrix_rows(), vs->flip_y());
}
void V3dGraphicsDevice::destroy_pipeline(IPipeline* pipeline) { destroy(allocator_, static_cast<V3dPipeline*>(pipeline)); }

ICommandBuffer* V3dGraphicsDevice::create_command_buffer() { return make<CommandRecorder>(allocator_, allocator_); }
void V3dGraphicsDevice::destroy_command_buffer(ICommandBuffer* commands) { destroy(allocator_, static_cast<CommandRecorder*>(commands)); }

bool V3dGraphicsDevice::run_pass(const RenderPassBegin& pass, const List<DrawRecord>& draws) {
  V3dImage* color = static_cast<V3dImage*>(pass.color);
  V3dImage* depth = static_cast<V3dImage*>(pass.depth);
  if (!color || !(color->usage() & kImageColorAttachment) || color->format() == Format::D32Float) return false;
  if (depth && (depth->format() != Format::D32Float || !(depth->usage() & kImageDepthAttachment) || depth->extent().width != color->extent().width ||
                depth->extent().height != color->extent().height))
    return false;
  if (!ready()) return false;

  V3dFrameDesc frame;
  frame.width = color->extent().width;
  frame.height = color->extent().height;
  frame.color = {color->block().gpu_address, color->stride_bytes()};
  frame.swap_rb = color->format() == Format::B8G8R8A8Unorm;
  frame.clear_color = pass.clear_color;
  frame.load_color = !pass.clear_color;
  const float clear_rgba[4] = {pass.clear_color_value.x, pass.clear_color_value.y, pass.clear_color_value.z, pass.clear_color_value.w};
  frame.clear_color_rgba8 = v3d_pack_rgba8(clear_rgba);
  if (depth) {
    frame.has_depth = true;
    frame.depth = {depth->block().gpu_address, depth->stride_bytes()};
    frame.clear_depth = pass.clear_depth;
    frame.load_depth = !pass.clear_depth;
    frame.store_depth = true;
    frame.clear_z_bits = v3d_float_bits(pass.clear_depth_value);
  }
  for (uint64_t i = 0; i < draws.count(); ++i)
    if (!draws[i].pipeline) return false;  // (a depth test with no depth image in the pass is left out of the state: it passes)

  V3dRenderLayout layout;
  const uint64_t n = draws.count();
  if (n > 100000) return false;
  layout.binner_bytes = align_up(static_cast<uint32_t>(1024 + n * 512), 4096);
  layout.indirect_bytes = align_up(static_cast<uint32_t>(512 + n * 768), 4096);
  uint64_t needed = 0;
  if (!V3dRenderer::memory_needed(frame, layout, &needed) || needed > UINT32_MAX - 4096) return false;
  V3dGpuBlock job;
  if (!memory_.allocate(static_cast<uint32_t>(needed), 4096, &job)) return false;
  V3dGpuBlock overflow;
  V3dMemoryBlock overflow_blocks[1];
  uint32_t overflow_count = 0;
  if (memory_.allocate(kOverflowBytes, 4096, &overflow)) overflow_blocks[overflow_count++] = {overflow.gpu_address, kOverflowBytes};

  PassContent content(draws, frame, code_, defaults_, shader_code_);
  V3dBlockJobMemory job_memory(memory_, job);
  // Vertex and index buffers were cleaned when they were unmapped; the images are only written by the GPU.
  const V3dJobResult result = V3dRenderer::render(gpu_, frame, job_memory, attempts_, ArrayView<const V3dMemoryBlock>(overflow_blocks, overflow_count), layout, &content);
  memory_.invalidate(color->block());
  if (depth) memory_.invalidate(depth->block());
  if (overflow.valid()) memory_.release(overflow);
  memory_.release(job);
  return result == V3dJobResult::Done;
}

bool V3dGraphicsDevice::submit(ICommandBuffer* commands) {
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
      case RecordedCommand::Type::BindPipeline: current.pipeline = static_cast<const V3dPipeline*>(cmd.pipeline); break;
      case RecordedCommand::Type::BindVertexBuffer: current.vertex_buffer = static_cast<V3dBuffer*>(cmd.buffer); current.vertex_offset = cmd.offset; break;
      case RecordedCommand::Type::BindIndexBuffer: current.index_buffer = static_cast<V3dBuffer*>(cmd.buffer); current.index_offset = cmd.offset; current.index_type = cmd.index_type; break;
      case RecordedCommand::Type::BindTexture: return false;   // (no pipeline of this device samples a texture)
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
      case RecordedCommand::Type::Resolve:
      case RecordedCommand::Type::BindComputePipeline:
      case RecordedCommand::Type::BindStorageBuffer:
      case RecordedCommand::Type::Dispatch:
      case RecordedCommand::Type::BeginQuery:
      case RecordedCommand::Type::EndQuery:
        return false;   // (no occlusion queries: DeviceInfo::features says so)
      case RecordedCommand::Type::SetTopology:
        if (cmd.topology != PrimitiveTopology::TriangleList) return false;   // (no lines or points here: DeviceInfo::features says so)
        break;
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

bool V3dGraphicsDevice::present(IImage* image) {
  V3dImage* img = static_cast<V3dImage*>(image);
  if (!img || !display_ || !(img->usage() & kImagePresent) || img->format() != Format::B8G8R8A8Unorm) return false;
  memory_.invalidate(img->block());
  const Surface s = img->surface();
  if (!s.valid()) return false;
  display_->blit(0, 0, s.width, s.height, s.pixels.data(), s.pitch);
  display_->present();
  return true;
}

}  // namespace graphics
