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

#include "Graphics/Graphics3D/Svga/SvgaDevice.hpp"
#include <new>
#include "Graphics/Graphics3D/Mipmaps.hpp"
#include "Utility/AllocatedView.hpp"

namespace graphics {

namespace {
// 3D command ids.
constexpr uint32_t kCmdSurfaceDefine = 1040, kCmdSurfaceDestroy = 1041, kCmdSurfaceDma = 1044, kCmdContextDefine = 1045, kCmdContextDestroy = 1046,
                   kCmdSetZRange = 1048, kCmdSetRenderState = 1049, kCmdSetRenderTarget = 1050, kCmdSetTextureState = 1051, kCmdSetViewport = 1055, kCmdClear = 1057,
                   kCmdPresent = 1058, kCmdShaderDefine = 1059, kCmdShaderDestroy = 1060, kCmdSetShader = 1061, kCmdSetShaderConst = 1062,
                   kCmdDrawPrimitives = 1063, kCmdSetScissorRect = 1064;

// Surfaces.
constexpr uint32_t kSurfaceHintTexture = 1u << 1, kSurfaceHintVertexBuffer = 1u << 2, kSurfaceHintIndexBuffer = 1u << 3, kSurfaceHintDepthStencil = 1u << 4,
                   kSurfaceHintRenderTarget = 1u << 5;
constexpr uint32_t kFormatA8R8G8B8 = 2, kFormatZD24S8 = 9, kFormatBuffer = 20;
constexpr uint32_t kTransferWrite = 1, kTransferRead = 2;             // guest -> host, host -> guest
constexpr uint32_t kGuestFramebuffer = 0xFFFFFFFEu;                    // "guest memory" that is the adapter's video memory
constexpr uint32_t kInvalidId = 0xFFFFFFFFu;

// Render states.
constexpr uint32_t kRsZEnable = 1, kRsZWriteEnable = 2, kRsBlendEnable = 5, kRsSrcBlend = 32, kRsDstBlend = 33, kRsBlendEquation = 34,
                   kRsCullMode = 35, kRsZFunc = 36, kRsFrontWinding = 43;
constexpr uint32_t kFaceNone = 1, kFaceFront = 2, kFaceBack = 3, kWindingCw = 1, kWindingCcw = 2;
constexpr uint32_t kBlendSrcAlpha = 5, kBlendInvSrcAlpha = 6, kBlendEquationAdd = 1, kCompareLessEqual = 4, kCompareAlways = 8;

constexpr uint32_t kShaderVertex = 1, kShaderPixel = 2;

// Texture states (SVGA3dTextureStateName) and their values: the sampler of a pixel shader's texture unit.
constexpr uint32_t kTsBindTexture = 1, kTsAddressU = 8, kTsAddressV = 9, kTsMipFilter = 10, kTsMagFilter = 11, kTsMinFilter = 12, kTsMipmapLevel = 21, kTsLodBias = 22;
constexpr uint32_t kAddressWrap = 1, kAddressClamp = 3;
constexpr uint32_t kFilterNone = 0, kFilterPoint = 1, kFilterLinear = 2;
constexpr uint32_t kRenderTargetDepth = 0, kRenderTargetColor0 = 2;
constexpr uint32_t kClearColor = 1, kClearDepth = 2;
constexpr uint32_t kPrimitiveTriangleList = 1;
constexpr uint32_t kUsagePosition = 0, kUsageTexCoord = 5, kUsageColor = 10;
constexpr uint32_t kFlipConstant = 31;

uint32_t float_bits(float f) {
  uint32_t u;
  __builtin_memcpy(&u, &f, 4);
  return u;
}

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

// --- Objects -------------------------------------------------------------------

bool define_surface(SvgaDevice& device, uint32_t id, uint32_t flags, uint32_t format, uint32_t width, uint32_t height, uint32_t levels = 1) {
  // The mip levels of the first face (none on the other five), then the size of each level.
  uint32_t body[9 + 3 * kMaxMipLevels] = {id, flags, format, levels, 0, 0, 0, 0, 0};
  for (uint32_t l = 0; l < levels && l < kMaxMipLevels; ++l) {
    body[9 + 3 * l] = mip_extent(width, l);
    body[10 + 3 * l] = mip_extent(height, l);
    body[11 + 3 * l] = 1;
  }
  return device.command(kCmdSurfaceDefine, body, 9 + 3 * levels);
}

void destroy_surface(SvgaDevice& device, uint32_t id) {
  if (id) device.command(kCmdSurfaceDestroy, &id, 1);
}

class SvgaBuffer final : public IBuffer {
 public:
  SvgaBuffer(SvgaDevice& device, uint64_t size, uint32_t usage) : device_(device) {
    memory_ = allocate_view<uint8_t>(device.allocator(), size, 16);
    if (memory_.empty()) return;
    memory_.fill(0);
    id_ = device.new_id();
    uint32_t flags = 0;
    if (usage & kBufferVertex) flags |= kSurfaceHintVertexBuffer;
    if (usage & kBufferIndex) flags |= kSurfaceHintIndexBuffer;
    ok_ = define_surface(device, id_, flags, kFormatBuffer, static_cast<uint32_t>(size), 1);
  }
  ~SvgaBuffer() {
    destroy_surface(device_, id_);
    release_view(device_.allocator(), memory_);
  }
  bool valid() const { return ok_; }
  uint32_t id() const { return id_; }
  uint64_t size() const override { return memory_.size(); }
  ByteView map() override { return memory_; }
  void unmap() override {
    // In pieces that fit the staging area.
    const uint32_t chunk = device_.scratch_bytes();
    for (uint64_t at = 0; at < memory_.size(); at += chunk) {
      const uint32_t n = static_cast<uint32_t>(memory_.size() - at < chunk ? memory_.size() - at : chunk);
      if (!device_.transfer(id_, true, static_cast<uint32_t>(at), n, 1, n, memory_.data() + at)) return;
    }
  }

 private:
  SvgaDevice& device_;
  ArrayView<uint8_t> memory_;
  uint32_t id_ = 0;
  bool ok_ = false;
};

class SvgaImage final : public IImage {
 public:
  SvgaImage(SvgaDevice& device, const ImageDesc& desc) : device_(device), desc_(desc) {
    id_ = device.new_id();
    const bool depth = desc.format == Format::D32Float;
    uint32_t flags = depth ? kSurfaceHintDepthStencil : (desc.usage & kImageColorAttachment) ? kSurfaceHintRenderTarget : 0;
    if (desc.usage & kImageSampled) flags |= kSurfaceHintTexture;
    if (!flags) flags = kSurfaceHintRenderTarget;
    ok_ = define_surface(device, id_, flags, depth ? kFormatZD24S8 : kFormatA8R8G8B8, desc.extent.width, desc.extent.height, desc.mip_levels);
  }
  ~SvgaImage() { destroy_surface(device_, id_); }
  bool valid() const { return ok_; }
  uint32_t id() const { return id_; }
  Extent2D extent() const override { return desc_.extent; }
  Format format() const override { return desc_.format; }
  uint32_t usage() const { return desc_.usage; }

  // A texture's pixels (0xAARRGGBB, rows top to bottom), a band of rows at a time.
  bool write_pixels(ArrayView<const uint32_t> pixels) override { return write_mip_pixels(0, pixels); }
  uint32_t mip_levels() const override { return desc_.mip_levels; }
  bool write_mip_pixels(uint32_t level, ArrayView<const uint32_t> pixels) override {
    if (desc_.format == Format::D32Float || !(desc_.usage & kImageSampled) || level >= desc_.mip_levels) return false;
    const uint32_t width = mip_extent(desc_.extent.width, level), height = mip_extent(desc_.extent.height, level);
    if (pixels.size() != uint64_t{width} * height) return false;
    const uint32_t rows = device_.scratch_bytes() / (width * 4);
    if (rows == 0) return false;
    for (uint32_t y = 0; y < height; y += rows) {
      const uint32_t n = height - y < rows ? height - y : rows;
      if (!device_.upload(id_, level, y, width, n, width * 4, &pixels[uint64_t{y} * width])) return false;
    }
    return true;
  }
  // Every level above 0 from level 0, by the shared box filter: level 0 is read back, each next level is made from the one before and sent.
  bool generate_mipmaps() override {
    if (desc_.format == Format::D32Float || !(desc_.usage & kImageSampled) || desc_.mip_levels < 2) return false;
    uint32_t width = desc_.extent.width, height = desc_.extent.height;
    ArrayView<uint32_t> above = allocate_view<uint32_t>(device_.allocator(), uint64_t{width} * height);
    if (above.empty()) return false;
    bool ok = true;
    const uint32_t rows = device_.scratch_bytes() / (width * 4);
    for (uint32_t y = 0; ok && rows != 0 && y < height; y += rows) {
      const uint32_t n = height - y < rows ? height - y : rows;
      ok = device_.download(id_, 0, y, width, n, width * 4, &above[uint64_t{y} * width]);
    }
    ok = ok && rows != 0;
    for (uint32_t level = 1; ok && level < desc_.mip_levels; ++level) {
      const uint32_t w = mip_extent(desc_.extent.width, level), h = mip_extent(desc_.extent.height, level);
      ArrayView<uint32_t> below = allocate_view<uint32_t>(device_.allocator(), uint64_t{w} * h);
      if (below.empty()) {
        ok = false;
        break;
      }
      mip_box_downsample(ArrayView<const uint32_t>(above.data(), above.size()), width, height, below, w, h);
      ok = write_mip_pixels(level, ArrayView<const uint32_t>(below.data(), below.size()));
      release_view(device_.allocator(), above);
      above = below;
      width = w, height = h;
    }
    release_view(device_.allocator(), above);
    return ok;
  }

  bool read_pixels(ArrayView<uint32_t> out) override {
    const uint32_t width = desc_.extent.width, height = desc_.extent.height;
    if (desc_.format == Format::D32Float || out.size() < uint64_t{width} * height) return false;   // (level 0)
    // A band of rows at a time, as many as the staging area holds.
    const uint32_t rows = device_.scratch_bytes() / (width * 4);
    if (rows == 0) return false;
    for (uint32_t y = 0; y < height; y += rows) {
      const uint32_t n = height - y < rows ? height - y : rows;
      if (!device_.download(id_, 0, y, width, n, width * 4, &out[uint64_t{y} * width])) return false;
    }
    return true;
  }

 private:
  SvgaDevice& device_;
  ImageDesc desc_;
  uint32_t id_ = 0;
  bool ok_ = false;
};

class SvgaShaderModule final : public IShaderModule {
 public:
  SvgaShaderModule(SvgaDevice& device, const ShaderModuleDesc& desc) : device_(device), stage_(desc.stage) {
    id_ = device.new_id();
    const uint32_t words = desc.d3d9_token_count;
    uint32_t* body = static_cast<uint32_t*>(device.allocator().allocate((3 + uint64_t{words}) * 4));
    if (!body) return;
    body[0] = SvgaDevice::kContext;
    body[1] = id_;
    body[2] = desc.stage == ShaderStage::Vertex ? kShaderVertex : kShaderPixel;
    __builtin_memcpy(body + 3, desc.d3d9_tokens, uint64_t{words} * 4);
    ok_ = device.command(kCmdShaderDefine, body, 3 + words);
    device.allocator().deallocate(body);
  }
  ~SvgaShaderModule() {
    if (id_) {
      const uint32_t body[] = {SvgaDevice::kContext, id_, stage_ == ShaderStage::Vertex ? kShaderVertex : kShaderPixel};
      device_.command(kCmdShaderDestroy, body, 3);
    }
  }
  bool valid() const { return ok_; }
  ShaderStage stage() const override { return stage_; }
  uint32_t id() const { return id_; }

 private:
  SvgaDevice& device_;
  ShaderStage stage_;
  uint32_t id_ = 0;
  bool ok_ = false;
};

class SvgaPipeline final : public IPipeline {
 public:
  explicit SvgaPipeline(const GraphicsPipelineDesc& desc)
      : desc_(desc), vs_(static_cast<const SvgaShaderModule*>(desc.vertex_shader)->id()),
        fs_(static_cast<const SvgaShaderModule*>(desc.fragment_shader)->id()) {}
  const GraphicsPipelineDesc& desc() const { return desc_; }
  uint32_t vertex_shader() const { return vs_; }
  uint32_t fragment_shader() const { return fs_; }

 private:
  GraphicsPipelineDesc desc_;
  uint32_t vs_, fs_;
};

}  // namespace

// --- Device --------------------------------------------------------------------

SvgaDevice::~SvgaDevice() {
  if (context_) {
    const uint32_t cid = kContext;
    command(kCmdContextDestroy, &cid, 1);
    gpu_.wait_idle();
  }
}

bool SvgaDevice::initialize() {
  scratch_ = gpu_.scratch(&scratch_offset_, &scratch_bytes_);
  if (!scratch_ || !gpu_.has_3d()) return false;
  const uint32_t cid = kContext;
  if (!command(kCmdContextDefine, &cid, 1)) return false;
  context_ = true;
  // The factor that turns Direct3D's +Y up into Vulkan's +Y down, for every vertex shader.
  const uint32_t flip[] = {kContext, kFlipConstant, kShaderVertex, 0, float_bits(1.0f), float_bits(-1.0f), float_bits(1.0f), float_bits(1.0f)};
  return command(kCmdSetShaderConst, flip, 8);
}

bool SvgaDevice::command(uint32_t id, const uint32_t* body, uint32_t words) {
  uint32_t stack[64];
  uint32_t* message = stack;
  uint32_t* heap = nullptr;
  if (words + 2 > 64) {
    heap = static_cast<uint32_t*>(allocator_.allocate((uint64_t{words} + 2) * 4));
    if (!heap) return false;
    message = heap;
  }
  message[0] = id;
  message[1] = words * 4;
  __builtin_memcpy(message + 2, body, uint64_t{words} * 4);
  const bool ok = gpu_.command(message, words + 2);
  allocator_.deallocate(heap);
  return ok;
}

// A transfer moves `height` rows of `width` units between the surface (starting at row/offset `x` - the byte offset for a buffer, the first row
// for an image) and the staging area, and waits for the host to finish.
bool SvgaDevice::transfer(uint32_t surface, bool write, uint32_t start, uint32_t width, uint32_t height, uint32_t pitch, void* guest_data) {
  return move(surface, 0, write, start, width, height, pitch, guest_data, guest_data);
}

bool SvgaDevice::move(uint32_t surface, uint32_t mip, bool write, uint32_t start, uint32_t width, uint32_t height, uint32_t pitch, const void* source, void* destination) {
  const uint64_t bytes = uint64_t{pitch} * height;
  if (!scratch_ || bytes > scratch_bytes_) return false;
  if (write) __builtin_memcpy(scratch_, source, bytes);
  const bool buffer = height == 1 && pitch == width;   // a 1D surface is addressed along x
  const uint32_t body[] = {
      kGuestFramebuffer, scratch_offset_, pitch,   // the guest side: video memory at the staging area
      surface, 0, mip,                              // the host side: face 0, the mip level
      write ? kTransferWrite : kTransferRead,
      buffer ? start : 0, buffer ? 0 : start, 0,    // the copy box: x, y, z
      width, height, 1,                             // size
      0, 0, 0,                                      // the position in the guest data
  };
  if (!command(kCmdSurfaceDma, body, 16)) return false;
  gpu_.wait_idle();
  if (!write) __builtin_memcpy(destination, scratch_, bytes);
  return true;
}

IBuffer* SvgaDevice::create_buffer(uint64_t size, uint32_t usage) {
  if (!size || size > 0x7FFFFFFF) return nullptr;
  SvgaBuffer* b = make<SvgaBuffer>(allocator_, *this, size, usage);
  if (b && !b->valid()) {
    destroy(allocator_, b);
    return nullptr;
  }
  return b;
}
void SvgaDevice::destroy_buffer(IBuffer* buffer) { destroy(allocator_, static_cast<SvgaBuffer*>(buffer)); }

IImage* SvgaDevice::create_image(const ImageDesc& desc) {
  if (desc.stencil) return nullptr;   // (no stencil plane on this device)
  if (desc.mip_levels == 0 || desc.mip_levels > kMaxMipLevels || desc.mip_levels > mip_level_limit(desc.extent)) return nullptr;
  if (desc.mip_levels > 1 && (desc.format == Format::D32Float || desc.usage != kImageSampled)) return nullptr;   // (only a texture has levels)
  if (!desc.extent.width || !desc.extent.height || desc.format == Format::Undefined || desc.format == Format::R8G8B8A8Unorm) return nullptr;
  SvgaImage* image = make<SvgaImage>(allocator_, *this, desc);
  if (image && !image->valid()) {
    destroy(allocator_, image);
    return nullptr;
  }
  return image;
}
void SvgaDevice::destroy_image(IImage* image) { destroy(allocator_, static_cast<SvgaImage*>(image)); }

IShaderModule* SvgaDevice::create_shader_module(const ShaderModuleDesc& desc) {
  if (!desc.d3d9_tokens || !desc.d3d9_token_count) return nullptr;
  SvgaShaderModule* m = make<SvgaShaderModule>(allocator_, *this, desc);
  if (m && !m->valid()) {
    destroy(allocator_, m);
    return nullptr;
  }
  return m;
}
void SvgaDevice::destroy_shader_module(IShaderModule* module) { destroy(allocator_, static_cast<SvgaShaderModule*>(module)); }

IPipeline* SvgaDevice::create_graphics_pipeline(const GraphicsPipelineDesc& desc) {
  if (desc.texture_count > kMaxTextureUnits || desc.fragment_shader == nullptr) return nullptr;
  if (desc.stencil.enable) return nullptr;   // (no stencil test on this device)
  if (!desc.vertex_shader || !desc.fragment_shader || desc.attribute_count > kMaxVertexAttributes || desc.varying_count > kMaxVaryings) return nullptr;
  if (desc.vertex_shader->stage() != ShaderStage::Vertex || desc.fragment_shader->stage() != ShaderStage::Fragment) return nullptr;
  return make<SvgaPipeline>(allocator_, desc);
}
void SvgaDevice::destroy_pipeline(IPipeline* pipeline) { destroy(allocator_, static_cast<SvgaPipeline*>(pipeline)); }

ICommandBuffer* SvgaDevice::create_command_buffer() { return make<CommandRecorder>(allocator_, allocator_); }
void SvgaDevice::destroy_command_buffer(ICommandBuffer* commands) { destroy(allocator_, static_cast<CommandRecorder*>(commands)); }

bool SvgaDevice::submit(ICommandBuffer* commands) {
  if (!commands) return false;
  const CommandRecorder& recorder = *static_cast<CommandRecorder*>(commands);

  const SvgaPipeline* pipeline = nullptr;
  SvgaImage* color = nullptr;
  SvgaImage* depth = nullptr;
  SvgaBuffer* vertex_buffer = nullptr;
  uint64_t vertex_offset = 0;
  SvgaBuffer* index_buffer = nullptr;
  uint64_t index_offset = 0;
  IndexType index_type = IndexType::Uint16;
  uint32_t constants[kMaxPushConstantBytes / 4] = {};
  uint32_t constants_dwords = 0;
  bool constants_dirty = false;
  bool in_pass = false;
  struct Bound {
    const SvgaImage* image = nullptr;
    SamplerDesc sampler;
  } textures[kMaxTextureUnits];
  bool textures_dirty = false;
  const ConstByteView pool = recorder.constants();
  const uint32_t cid = kContext;

  auto viewport = [&](const Viewport& v) {
    const uint32_t rect[] = {cid, static_cast<uint32_t>(v.x), static_cast<uint32_t>(v.y), static_cast<uint32_t>(v.width), static_cast<uint32_t>(v.height)};
    const uint32_t z[] = {cid, float_bits(v.min_depth), float_bits(v.max_depth)};
    command(kCmdSetViewport, rect, 5);
    command(kCmdSetZRange, z, 3);
  };

  for (uint64_t i = 0; i < recorder.commands().count(); ++i) {
    const RecordedCommand& cmd = recorder.commands()[i];
    switch (cmd.type) {
      case RecordedCommand::Type::BeginRenderPass: {
        if (in_pass || !cmd.pass.color) return false;
        color = static_cast<SvgaImage*>(cmd.pass.color);
        depth = static_cast<SvgaImage*>(cmd.pass.depth);
        if (color->format() == Format::D32Float) return false;
        if (depth && depth->format() != Format::D32Float) return false;
        const Extent2D e = color->extent();
        const uint32_t targets[2][5] = {{cid, kRenderTargetColor0, color->id(), 0, 0},
                                        {cid, kRenderTargetDepth, depth ? depth->id() : kInvalidId, 0, 0}};
        command(kCmdSetRenderTarget, targets[0], 5);
        command(kCmdSetRenderTarget, targets[1], 5);
        viewport({0, 0, static_cast<float>(e.width), static_cast<float>(e.height), 0, 1});
        uint32_t flags = 0;
        if (cmd.pass.clear_color) flags |= kClearColor;
        if (depth && cmd.pass.clear_depth) flags |= kClearDepth;
        if (flags) {
          auto channel = [](float f) { return static_cast<uint32_t>((f < 0 ? 0 : f > 1 ? 1 : f) * 255.0f + 0.5f); };
          const Vec4& c = cmd.pass.clear_color_value;
          const uint32_t argb = (channel(c.w) << 24) | (channel(c.x) << 16) | (channel(c.y) << 8) | channel(c.z);
          const uint32_t clear[] = {cid, flags, argb, float_bits(cmd.pass.clear_depth_value), 0, 0, 0, e.width, e.height};
          command(kCmdClear, clear, 9);
        }
        pipeline = nullptr;
        in_pass = true;
        break;
      }
      case RecordedCommand::Type::EndRenderPass:
        in_pass = false;
        break;
      case RecordedCommand::Type::BindPipeline: {
        pipeline = static_cast<const SvgaPipeline*>(cmd.pipeline);
        if (!pipeline) return false;
        const GraphicsPipelineDesc& d = pipeline->desc();
        const uint32_t cull = d.cull_mode == CullMode::None ? kFaceNone : d.cull_mode == CullMode::Back ? kFaceBack : kFaceFront;
        const bool z_on = d.depth_test || d.depth_write;
        const uint32_t states[] = {
            cid,
            kRsZEnable, z_on ? 1u : 0u,
            kRsZWriteEnable, d.depth_write ? 1u : 0u,
            kRsZFunc, d.depth_test ? kCompareLessEqual : kCompareAlways,
            kRsBlendEnable, d.alpha_blend ? 1u : 0u,
            kRsSrcBlend, kBlendSrcAlpha,
            kRsDstBlend, kBlendInvSrcAlpha,
            kRsBlendEquation, kBlendEquationAdd,
            kRsCullMode, cull,
            kRsFrontWinding, d.front_face == FrontFace::CounterClockwise ? kWindingCcw : kWindingCw,
        };
        command(kCmdSetRenderState, states, sizeof states / 4);
        const uint32_t vs[] = {cid, kShaderVertex, pipeline->vertex_shader()};
        const uint32_t ps[] = {cid, kShaderPixel, pipeline->fragment_shader()};
        command(kCmdSetShader, vs, 3);
        command(kCmdSetShader, ps, 3);
        constants_dirty = constants_dwords != 0;
        break;
      }
      case RecordedCommand::Type::BindVertexBuffer:
        vertex_buffer = static_cast<SvgaBuffer*>(cmd.buffer);
        vertex_offset = cmd.offset;
        break;
      case RecordedCommand::Type::BindIndexBuffer:
        index_buffer = static_cast<SvgaBuffer*>(cmd.buffer);
        index_offset = cmd.offset;
        index_type = cmd.index_type;
        break;
      case RecordedCommand::Type::BindTexture: {
        const SvgaImage* image = static_cast<const SvgaImage*>(cmd.image);
        if (cmd.unit >= kMaxTextureUnits || !image || !(image->usage() & kImageSampled) || image->format() == Format::D32Float) return false;
        textures[cmd.unit].image = image;
        textures[cmd.unit].sampler = cmd.sampler;
        textures_dirty = true;
        break;
      }
      case RecordedCommand::Type::PushConstants: {
        ConstByteView block;
        if (!pool.try_subview(cmd.constants_offset, cmd.constants_size, &block)) return false;
        for (uint32_t k = 0; k < block.size(); ++k) reinterpret_cast<uint8_t*>(constants)[k] = block[k];
        constants_dwords = cmd.constants_size / 4;
        constants_dirty = true;
        break;
      }
      case RecordedCommand::Type::SetViewport:
        viewport(cmd.viewport);
        break;
      case RecordedCommand::Type::SetScissor: {
        if (!in_pass) return false;
        const Extent2D e = color->extent();
        const int64_t x0 = cmd.scissor.x > 0 ? cmd.scissor.x : 0, y0 = cmd.scissor.y > 0 ? cmd.scissor.y : 0;
        int64_t x1 = int64_t{cmd.scissor.x} + cmd.scissor.width, y1 = int64_t{cmd.scissor.y} + cmd.scissor.height;
        if (x1 > e.width) x1 = e.width;
        if (y1 > e.height) y1 = e.height;
        const uint32_t rect[] = {cid, static_cast<uint32_t>(x0), static_cast<uint32_t>(y0), x1 > x0 ? static_cast<uint32_t>(x1 - x0) : 0,
                                 y1 > y0 ? static_cast<uint32_t>(y1 - y0) : 0};
        command(kCmdSetScissorRect, rect, 5);
        break;
      }
      case RecordedCommand::Type::Draw:
      case RecordedCommand::Type::DrawIndexed: {
        if (!in_pass || !pipeline || !vertex_buffer) return false;
        const bool indexed = cmd.type == RecordedCommand::Type::DrawIndexed;
        if (indexed && !index_buffer) return false;
        if (constants_dirty) {
          // (the push constants are the first registers of both stages)
          for (uint32_t reg = 0; reg < constants_dwords / 4; ++reg) {
            const uint32_t stages[2] = {kShaderVertex, kShaderPixel};
            for (uint32_t stage : stages) {
              const uint32_t body[] = {cid, reg, stage, 0, constants[4 * reg], constants[4 * reg + 1], constants[4 * reg + 2], constants[4 * reg + 3]};
              command(kCmdSetShaderConst, body, 8);
            }
          }
          constants_dirty = false;
        }
        const uint32_t texture_count = pipeline->desc().texture_count;
        for (uint32_t u = 0; u < texture_count; ++u)
          if (!textures[u].image || textures[u].image == color || textures[u].image == depth) return false;   // (unbound, or drawn into by this pass)
        if (textures_dirty && texture_count) {
          for (uint32_t u = 0; u < texture_count; ++u) {
            const SamplerDesc& sd = textures[u].sampler;
            const uint32_t filter = sd.filter == Filter::Linear ? kFilterLinear : kFilterPoint;
            const bool mipped = textures[u].image->mip_levels() > 1 && sd.mip_filter != MipFilter::None;
            const uint32_t mip_filter = !mipped ? kFilterNone : sd.mip_filter == MipFilter::Nearest ? kFilterPoint : kFilterLinear;
            const float top = static_cast<float>(textures[u].image->mip_levels() - 1);
            const uint32_t most_detailed = static_cast<uint32_t>(sd.min_lod <= 0 ? 0.0f : sd.min_lod > top ? top : sd.min_lod);
            auto address = [](AddressMode m) { return m == AddressMode::Repeat ? kAddressWrap : kAddressClamp; };
            const uint32_t body[] = {cid,
                                     u, kTsBindTexture, textures[u].image->id(),
                                     u, kTsAddressU, address(sd.address_u),
                                     u, kTsAddressV, address(sd.address_v),
                                     u, kTsMagFilter, filter,
                                     u, kTsMinFilter, filter,
                                     u, kTsMipFilter, mip_filter,
                                     u, kTsMipmapLevel, most_detailed,
                                     u, kTsLodBias, float_bits(mipped ? sd.lod_bias : 0.0f)};
            command(kCmdSetTextureState, body, sizeof body / 4);
          }
          textures_dirty = false;
        }
        const GraphicsPipelineDesc& d = pipeline->desc();
        // The vertex declarations: each attribute is an array in the vertex buffer. Location 0 is the position and 1 the color by convention
        // (what the shaders declare); further locations are texture coordinates.
        uint32_t body[3 + 9 * kMaxVertexAttributes + 7] = {cid, d.attribute_count, 1};
        uint32_t n = 3;
        const uint32_t vertex_count = cmd.count;
        const uint64_t first_vertex = indexed ? 0 : cmd.first;
        const uint32_t hint_last = indexed ? static_cast<uint32_t>(d.vertex_stride ? (vertex_buffer->size() - vertex_offset) / d.vertex_stride : 0) : vertex_count;
        for (uint32_t a = 0; a < d.attribute_count; ++a) {
          const VertexAttribute& attribute = d.attributes[a];
          const uint32_t usage = attribute.location == 0 ? kUsagePosition : attribute.location == 1 ? kUsageColor : kUsageTexCoord;
          const uint32_t usage_index = attribute.location >= 2 ? attribute.location - 2 : 0;
          body[n++] = static_cast<uint32_t>(attribute.format);   // float1..float4 are 0..3
          body[n++] = 0;                                           // default method
          body[n++] = usage;
          body[n++] = usage_index;
          body[n++] = vertex_buffer->id();
          body[n++] = static_cast<uint32_t>(vertex_offset + first_vertex * d.vertex_stride + attribute.offset);
          body[n++] = d.vertex_stride;
          body[n++] = 0;
          body[n++] = hint_last;
        }
        // The one primitive range: triangles, indexed or not.
        body[n++] = kPrimitiveTriangleList;
        body[n++] = vertex_count / 3;
        if (indexed) {
          const uint32_t width = index_type == IndexType::Uint16 ? 2 : 4;
          body[n++] = index_buffer->id();
          body[n++] = static_cast<uint32_t>(index_offset + uint64_t{cmd.first} * width);
          body[n++] = width;
          body[n++] = width;
          body[n++] = static_cast<uint32_t>(cmd.vertex_offset);
        } else {
          body[n++] = kInvalidId;
          body[n++] = 0;
          body[n++] = 0;
          body[n++] = 0;
          body[n++] = 0;
        }
        command(kCmdDrawPrimitives, body, n);
        break;
      }
    }
  }
  if (in_pass) return false;
  gpu_.wait_idle();
  return true;
}

bool SvgaDevice::present(IImage* image) {
  auto* img = static_cast<SvgaImage*>(image);
  if (!img || !(img->usage() & kImagePresent)) return false;
  const Extent2D e = img->extent();
  const uint32_t body[] = {img->id(), 0, 0, 0, 0, e.width, e.height};   // one copy rectangle: x, y, source x, source y, size
  return command(kCmdPresent, body, 7);
}

}  // namespace graphics
