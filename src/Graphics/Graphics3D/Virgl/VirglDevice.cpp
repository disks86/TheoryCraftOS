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

#include "Graphics/Graphics3D/Virgl/VirglDevice.hpp"
#include <new>
#include "Graphics/Graphics3D/Virgl/VirglProtocol.hpp"
#include "Utility/AllocatedView.hpp"

namespace graphics {

using namespace hardware::virtio_gpu;

namespace {

template <class T>
ConstByteView as_bytes(const T& object) {
  return ConstByteView(reinterpret_cast<const uint8_t*>(&object), sizeof(T));
}

ControlHeader header(uint32_t type) {
  ControlHeader h{};
  h.type = type;
  return h;
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

uint32_t float_bits(float f) {
  uint32_t u;
  __builtin_memcpy(&u, &f, 4);
  return u;
}

uint32_t string_length(const char* s) {
  uint32_t n = 0;
  while (s[n]) ++n;
  return n;
}

// Builds a virgl command stream in a fixed array; the caller flushes it through the device.
class Stream {
 public:
  Stream(allocators::IAllocator& allocator) : allocator_(allocator) {
    data_ = allocate_view<uint32_t>(allocator, kCapacity, 16);   // (the stream is copied into the request, not read by the device)
  }
  ~Stream() { release_view(allocator_, data_); }
  bool valid() const { return !data_.empty(); }
  uint32_t count() const { return count_; }
  const uint32_t* data() const { return data_.data(); }
  void reset() { count_ = 0; }
  // Room for `dwords` more (including the header), or the stream has overflowed.
  bool command(uint32_t cmd, uint32_t object, uint32_t length) {
    if (count_ + length + 1 > kCapacity) {
      overflow_ = true;
      return false;
    }
    data_[count_++] = virgl::header(cmd, object, length);
    return true;
  }
  void put(uint32_t v) {
    if (count_ < kCapacity) data_[count_++] = v;
  }
  void putf(float f) { put(float_bits(f)); }
  bool overflow() const { return overflow_; }

  static constexpr uint32_t kCapacity = 24 * 1024;

 private:
  allocators::IAllocator& allocator_;
  ArrayView<uint32_t> data_;
  uint32_t count_ = 0;
  bool overflow_ = false;
};

// --- Objects -------------------------------------------------------------------

class VirglBuffer final : public IBuffer {
 public:
  VirglBuffer(VirglDevice& device, uint64_t size, uint32_t usage) : device_(device) {
    hardware::VirtioGpu& gpu = device.gpu();
    memory_ = allocate_dma_view<uint8_t>(gpu.allocator(), size, 4096);
    if (memory_.empty()) return;
    memory_.fill(0);
    id_ = gpu.allocate_resource_id();
    uint32_t bind = 0;
    if (usage & kBufferVertex) bind |= virgl::kBindVertexBuffer;
    if (usage & kBufferIndex) bind |= virgl::kBindIndexBuffer;
    ResourceCreate3d create{header(kCmdResourceCreate3d), id_, virgl::kTargetBuffer, virgl::kFormatR8Unorm, bind,
                            static_cast<uint32_t>(size), 1, 1, 1, 0, 0, 0, 0};
    ResourceAttachBacking attach{header(kCmdResourceAttachBacking), id_, 1,
                                 {reinterpret_cast<uint64_t>(memory_.data()), static_cast<uint32_t>(size), 0}};
    CtxResource ctx{header(kCmdCtxAttachResource), id_, 0};
    ok_ = gpu.command(as_bytes(create)) && gpu.command(as_bytes(attach)) && gpu.command(as_bytes(ctx));
    gpu.add_resource_bytes(size);
    accounted_ = size;
  }
  ~VirglBuffer() {
    hardware::VirtioGpu& gpu = device_.gpu();
    if (id_) {
      CtxResource detach{header(kCmdCtxDetachResource), id_, 0};
      ResourceUnref unref{header(kCmdResourceUnref), id_, 0};
      gpu.command(as_bytes(detach));
      gpu.command(as_bytes(unref));
    }
    gpu.remove_resource_bytes(accounted_);
    release_view(gpu.allocator(), memory_);
  }
  bool valid() const { return ok_; }
  uint32_t id() const { return id_; }
  uint64_t size() const override { return memory_.size(); }
  ByteView map() override { return memory_; }
  void unmap() override {
    TransferHost3d transfer{header(kCmdTransferToHost3d), {0, 0, 0, static_cast<uint32_t>(memory_.size()), 1, 1}, 0,
                            id_, 0, 0, 0};
    device_.gpu().command(as_bytes(transfer));
  }

 private:
  VirglDevice& device_;
  ByteView memory_;
  uint32_t id_ = 0;
  uint64_t accounted_ = 0;   // what the GPU's video memory figure counts for this buffer
  bool ok_ = false;
};

class VirglImage final : public IImage {
 public:
  VirglImage(VirglDevice& device, const ImageDesc& desc) : device_(device), desc_(desc) {
    hardware::VirtioGpu& gpu = device.gpu();
    id_ = gpu.allocate_resource_id();
    const bool depth = desc.format == Format::D32Float;
    uint32_t bind = depth ? virgl::kBindDepthStencil : (virgl::kBindRenderTarget | virgl::kBindSamplerView);
    if (desc.usage & kImagePresent) bind |= virgl::kBindScanout;
    ResourceCreate3d create{header(kCmdResourceCreate3d), id_, virgl::kTargetTexture2d,
                            depth ? virgl::kFormatZ32Float : virgl::kFormatB8G8R8A8Unorm, bind,
                            desc.extent.width, desc.extent.height, 1, 1, 0, 0, kResourceFlagY0Top, 0};
    CtxResource ctx{header(kCmdCtxAttachResource), id_, 0};
    ok_ = gpu.command(as_bytes(create)) && gpu.command(as_bytes(ctx));
    accounted_ = uint64_t{desc.extent.width} * desc.extent.height * 4;   // (every format here is 32 bits per pixel)
    gpu.add_resource_bytes(accounted_);
    // The surface (a render-target view of the resource) is what the framebuffer state refers to.
    if (ok_ && (desc.usage & (kImageColorAttachment | kImageDepthAttachment))) {
      surface_ = device.new_handle();
      const uint32_t fmt = depth ? virgl::kFormatZ32Float : virgl::kFormatB8G8R8A8Unorm;
      const uint32_t cmd[] = {virgl::header(virgl::kCreateObject, virgl::kObjectSurface, 5), surface_, id_, fmt, 0, 0};
      ok_ = device.send_commands(cmd, 6);
    }
  }
  ~VirglImage() {
    hardware::VirtioGpu& gpu = device_.gpu();
    if (surface_) device_.destroy_object(surface_);
    if (id_) {
      CtxResource detach{header(kCmdCtxDetachResource), id_, 0};
      ResourceUnref unref{header(kCmdResourceUnref), id_, 0};
      gpu.command(as_bytes(detach));
      gpu.command(as_bytes(unref));
    }
    gpu.remove_resource_bytes(accounted_);
    release_view(gpu.allocator(), backing_);
  }
  bool valid() const { return ok_; }
  uint32_t id() const { return id_; }
  uint32_t surface() const { return surface_; }
  Extent2D extent() const override { return desc_.extent; }
  Format format() const override { return desc_.format; }
  uint32_t usage() const { return desc_.usage; }

  bool read_pixels(ArrayView<uint32_t> out) override {
    const uint64_t count = uint64_t{desc_.extent.width} * desc_.extent.height;
    if (desc_.format == Format::D32Float || out.size() < count) return false;
    hardware::VirtioGpu& gpu = device_.gpu();
    if (backing_.empty()) {  // guest memory for the host to copy into, attached on first use
      backing_ = allocate_dma_view<uint8_t>(gpu.allocator(), count * 4, 4096);
      if (backing_.empty()) return false;
      ResourceAttachBacking attach{header(kCmdResourceAttachBacking), id_, 1,
                                   {reinterpret_cast<uint64_t>(backing_.data()), static_cast<uint32_t>(count * 4), 0}};
      if (!gpu.command(as_bytes(attach))) {
        release_view(gpu.allocator(), backing_);
        return false;
      }
    }
    TransferHost3d transfer{header(kCmdTransferFromHost3d),
                            {0, 0, 0, desc_.extent.width, desc_.extent.height, 1}, 0, id_, 0,
                            desc_.extent.width * 4, 0};
    if (!gpu.command(as_bytes(transfer))) return false;
    const ArrayView<uint32_t> pixels = backing_.cast<uint32_t>();
    for (uint64_t i = 0; i < count; ++i) out[i] = pixels[i];
    return true;
  }

 private:
  VirglDevice& device_;
  ImageDesc desc_;
  uint32_t id_ = 0, surface_ = 0;
  uint64_t accounted_ = 0;   // what the GPU's video memory figure counts for this image
  ByteView backing_;
  bool ok_ = false;
};

class VirglShaderModule final : public IShaderModule {
 public:
  VirglShaderModule(VirglDevice& device, const ShaderModuleDesc& desc) : stage_(desc.stage) {
    handle_ = device.new_handle();
    const uint32_t length = string_length(desc.tgsi) + 1;  // with the terminating NUL
    const uint32_t text_dwords = (length + 3) / 4;
    const uint32_t total = 6 + text_dwords;
    hardware::VirtioGpu& gpu = device.gpu();
    uint32_t* cmd = static_cast<uint32_t*>(gpu.allocator().allocate(total * 4));
    if (!cmd) return;
    cmd[0] = virgl::header(virgl::kCreateObject, virgl::kObjectShader, 5 + text_dwords);
    cmd[1] = handle_;
    cmd[2] = desc.stage == ShaderStage::Vertex ? virgl::kShaderVertex : virgl::kShaderFragment;
    cmd[3] = length;                         // total text length (no continuation bit: it all fits)
    cmd[4] = length < 300 ? 300 : length;    // upper bound on the TGSI token count the host reserves
    cmd[5] = 0;                              // no stream output
    cmd[6 + text_dwords - 1] = 0;
    __builtin_memcpy(&cmd[6], desc.tgsi, length - 1);
    ok_ = device.send_commands(cmd, total);
    gpu.allocator().deallocate(cmd);
  }
  bool valid() const { return ok_; }
  ShaderStage stage() const override { return stage_; }
  uint32_t handle() const { return handle_; }

 private:
  ShaderStage stage_;
  uint32_t handle_ = 0;
  bool ok_ = false;
};

class VirglPipeline final : public IPipeline {
 public:
  VirglPipeline(VirglDevice& device, const GraphicsPipelineDesc& desc)
      : device_(device), desc_(desc), vs_(static_cast<const VirglShaderModule*>(desc.vertex_shader)->handle()),
        fs_(static_cast<const VirglShaderModule*>(desc.fragment_shader)->handle()) {
    blend_ = device.new_handle();
    dsa_ = device.new_handle();
    rasterizer_ = device.new_handle();
    elements_ = device.new_handle();
    uint32_t cmd[64];
    uint32_t n = 0;

    // Blend state: one render target, optional source-over blending.
    const uint32_t blend_rt =
        desc.alpha_blend
            ? (1u | (virgl::kBlendAdd << 1) | (virgl::kBlendFactorSrcAlpha << 4) | (virgl::kBlendFactorInvSrcAlpha << 9) |
               (virgl::kBlendAdd << 14) | (virgl::kBlendFactorOne << 17) | (virgl::kBlendFactorInvSrcAlpha << 22) |
               (0xFu << 27))
            : (0xFu << 27);
    cmd[n++] = virgl::header(virgl::kCreateObject, virgl::kObjectBlend, 11);
    cmd[n++] = blend_;
    cmd[n++] = 0;  // s0: no independent blend, logic op, dither...
    cmd[n++] = 0;  // s1
    cmd[n++] = blend_rt;
    for (int i = 1; i < 8; ++i) cmd[n++] = 0;

    // Depth/stencil/alpha: depth only.
    const uint32_t depth_func = desc.depth_test ? virgl::kFuncLequal : virgl::kFuncAlways;
    const bool depth_on = desc.depth_test || desc.depth_write;
    cmd[n++] = virgl::header(virgl::kCreateObject, virgl::kObjectDsa, 5);
    cmd[n++] = dsa_;
    cmd[n++] = (depth_on ? 1u : 0u) | ((desc.depth_write ? 1u : 0u) << 1) | (depth_func << 2);
    cmd[n++] = 0;
    cmd[n++] = 0;
    cmd[n++] = 0;

    // Rasterizer: depth clip with a 0..1 clip volume (Vulkan), scissor on, GL pixel conventions.
    // Y_0_TOP images make window space match Vulkan's, which flips what GL calls counter-clockwise.
    const uint32_t cull = desc.cull_mode == CullMode::None ? virgl::kFaceNone
                          : desc.cull_mode == CullMode::Back ? virgl::kFaceBack : virgl::kFaceFront;
    const bool front_ccw = desc.front_face == FrontFace::CounterClockwise;
    cmd[n++] = virgl::header(virgl::kCreateObject, virgl::kObjectRasterizer, 9);
    cmd[n++] = rasterizer_;
    cmd[n++] = (1u << 1) /*depth clip*/ | (1u << 2) /*clip half z*/ | (cull << 8) | (1u << 14) /*scissor*/ |
               ((front_ccw ? 1u : 0u) << 15) | (1u << 29) /*half pixel center*/ | (1u << 30) /*bottom edge rule*/;
    cmd[n++] = float_bits(1.0f);  // point size
    cmd[n++] = 0;                 // sprite coord enable
    cmd[n++] = 0;                 // line stipple, clip planes
    cmd[n++] = float_bits(1.0f);  // line width
    cmd[n++] = 0;
    cmd[n++] = 0;
    cmd[n++] = 0;

    // Vertex elements, all from vertex buffer 0.
    cmd[n++] = virgl::header(virgl::kCreateObject, virgl::kObjectVertexElements, desc.attribute_count * 4 + 1);
    cmd[n++] = elements_;
    for (uint32_t i = 0; i < desc.attribute_count; ++i) {
      static const uint32_t kFormats[] = {virgl::kFormatR32Float, virgl::kFormatR32G32Float,
                                          virgl::kFormatR32G32B32Float, virgl::kFormatR32G32B32A32Float};
      cmd[n++] = desc.attributes[i].offset;
      cmd[n++] = 0;  // instance divisor
      cmd[n++] = 0;  // vertex buffer index
      cmd[n++] = kFormats[static_cast<uint32_t>(desc.attributes[i].format)];
    }
    ok_ = n <= 64 && device.send_commands(cmd, n);
  }
  ~VirglPipeline() {
    device_.destroy_object(blend_);
    device_.destroy_object(dsa_);
    device_.destroy_object(rasterizer_);
    device_.destroy_object(elements_);
  }
  bool valid() const { return ok_; }
  const GraphicsPipelineDesc& desc() const { return desc_; }
  uint32_t blend() const { return blend_; }
  uint32_t dsa() const { return dsa_; }
  uint32_t rasterizer() const { return rasterizer_; }
  uint32_t elements() const { return elements_; }
  uint32_t vertex_shader() const { return vs_; }
  uint32_t fragment_shader() const { return fs_; }

 private:
  VirglDevice& device_;
  GraphicsPipelineDesc desc_;
  uint32_t vs_, fs_;
  uint32_t blend_ = 0, dsa_ = 0, rasterizer_ = 0, elements_ = 0;
  bool ok_ = false;
};

}  // namespace

// --- Device --------------------------------------------------------------------

bool VirglDevice::send_commands(const uint32_t* dwords, uint32_t count) {
  const uint64_t bytes = uint64_t{count} * 4;
  hardware::VirtioGpu& g = gpu_;
  uint8_t* request = static_cast<uint8_t*>(g.allocator().allocate(sizeof(CmdSubmit) + bytes));
  if (!request) return false;
  CmdSubmit submit{header(kCmdSubmit3d), static_cast<uint32_t>(bytes), 0};
  __builtin_memcpy(request, &submit, sizeof(submit));
  __builtin_memcpy(request + sizeof(submit), dwords, bytes);
  const bool ok = g.command(ConstByteView(request, sizeof(submit) + bytes));
  g.allocator().deallocate(request);
  return ok;
}

bool VirglDevice::destroy_object(uint32_t handle) {
  if (!handle) return true;
  const uint32_t cmd[] = {virgl::header(virgl::kDestroyObject, 0, 1), handle};
  return send_commands(cmd, 2);
}

IBuffer* VirglDevice::create_buffer(uint64_t size, uint32_t usage) {
  if (!size || size > 0x7FFFFFFF) return nullptr;
  VirglBuffer* b = make<VirglBuffer>(gpu_.allocator(), *this, size, usage);
  if (b && !b->valid()) {
    destroy(gpu_.allocator(), b);
    return nullptr;
  }
  return b;
}
void VirglDevice::destroy_buffer(IBuffer* buffer) { destroy(gpu_.allocator(), static_cast<VirglBuffer*>(buffer)); }

IImage* VirglDevice::create_image(const ImageDesc& desc) {
  if (desc.stencil) return nullptr;   // (no stencil plane on this device)
  if (!desc.extent.width || !desc.extent.height || desc.format == Format::Undefined) return nullptr;
  if (desc.format == Format::R8G8B8A8Unorm) return nullptr;  // only BGRA color images for now
  VirglImage* image = make<VirglImage>(gpu_.allocator(), *this, desc);
  if (image && !image->valid()) {
    destroy(gpu_.allocator(), image);
    return nullptr;
  }
  return image;
}
void VirglDevice::destroy_image(IImage* image) { destroy(gpu_.allocator(), static_cast<VirglImage*>(image)); }

IShaderModule* VirglDevice::create_shader_module(const ShaderModuleDesc& desc) {
  if (!desc.tgsi) return nullptr;
  VirglShaderModule* m = make<VirglShaderModule>(gpu_.allocator(), *this, desc);
  if (m && !m->valid()) {
    destroy(gpu_.allocator(), m);
    return nullptr;
  }
  return m;
}
void VirglDevice::destroy_shader_module(IShaderModule* module) {
  auto* m = static_cast<VirglShaderModule*>(module);
  if (m) destroy_object(m->handle());
  destroy(gpu_.allocator(), m);
}

IPipeline* VirglDevice::create_graphics_pipeline(const GraphicsPipelineDesc& desc) {
  if (desc.texture_count != 0 || desc.fragment_shader == nullptr) return nullptr;   // (no textures on this device)
  if (desc.stencil.enable) return nullptr;   // (no stencil test on this device)
  if (!desc.vertex_shader || !desc.fragment_shader || desc.attribute_count > kMaxVertexAttributes ||
      desc.varying_count > kMaxVaryings)
    return nullptr;
  if (desc.vertex_shader->stage() != ShaderStage::Vertex || desc.fragment_shader->stage() != ShaderStage::Fragment)
    return nullptr;
  VirglPipeline* p = make<VirglPipeline>(gpu_.allocator(), *this, desc);
  if (p && !p->valid()) {
    destroy(gpu_.allocator(), p);
    return nullptr;
  }
  return p;
}
void VirglDevice::destroy_pipeline(IPipeline* pipeline) { destroy(gpu_.allocator(), static_cast<VirglPipeline*>(pipeline)); }

ICommandBuffer* VirglDevice::create_command_buffer() { return make<CommandRecorder>(gpu_.allocator(), gpu_.allocator()); }
void VirglDevice::destroy_command_buffer(ICommandBuffer* commands) {
  destroy(gpu_.allocator(), static_cast<CommandRecorder*>(commands));
}

bool VirglDevice::submit(ICommandBuffer* commands) {
  if (!commands) return false;
  const CommandRecorder& recorder = *static_cast<CommandRecorder*>(commands);
  Stream stream(gpu_.allocator());
  if (!stream.valid()) return false;

  const VirglPipeline* pipeline = nullptr;
  VirglImage* color = nullptr;
  VirglImage* depth = nullptr;
  VirglBuffer* vertex_buffer = nullptr;
  uint64_t vertex_offset = 0;
  VirglBuffer* index_buffer = nullptr;
  uint64_t index_offset = 0;
  IndexType index_type = IndexType::Uint16;
  Viewport viewport;
  Rect2D scissor;
  uint32_t constants[kMaxPushConstantBytes / 4] = {};
  uint32_t constants_dwords = 0;
  bool constants_dirty = false;
  bool in_pass = false;
  const ConstByteView pool = recorder.constants();

  auto flush = [&]() {
    if (stream.count() && !send_commands(stream.data(), stream.count())) return false;
    stream.reset();
    return true;
  };
  auto emit_viewport = [&]() {
    if (!stream.command(virgl::kSetViewportState, 0, 7)) return;
    stream.put(0);
    stream.putf(viewport.width * 0.5f);
    stream.putf(-viewport.height * 0.5f);  // GL's +Y is up: flip to Vulkan's +Y down
    stream.putf(viewport.max_depth - viewport.min_depth);
    stream.putf(viewport.x + viewport.width * 0.5f);
    stream.putf(viewport.y + viewport.height * 0.5f);
    stream.putf(viewport.min_depth);
  };
  auto emit_scissor = [&]() {
    if (!stream.command(virgl::kSetScissorState, 0, 3)) return;
    stream.put(0);
    // Scissor rectangles are counted from the bottom of the target in virgl (GL's window space), while Vulkan's - and ours - count from the top.
    const uint32_t height = color ? color->extent().height : 0;
    const uint32_t y0 = height - (scissor.y + scissor.height), y1 = height - scissor.y;
    stream.put(scissor.x | (y0 << 16));
    stream.put((scissor.x + scissor.width) | (y1 << 16));
  };

  for (uint64_t i = 0; i < recorder.commands().count(); ++i) {
    const RecordedCommand& cmd = recorder.commands()[i];
    switch (cmd.type) {
      case RecordedCommand::Type::BeginRenderPass: {
        if (in_pass || !cmd.pass.color) return false;
        color = static_cast<VirglImage*>(cmd.pass.color);
        depth = static_cast<VirglImage*>(cmd.pass.depth);
        if (!color->surface() || color->format() == Format::D32Float) return false;
        if (depth && (depth->format() != Format::D32Float || !depth->surface())) return false;
        const Extent2D e = color->extent();
        stream.command(virgl::kSetFramebufferState, 0, 3);
        stream.put(1);
        stream.put(depth ? depth->surface() : 0);
        stream.put(color->surface());
        viewport = {0, 0, static_cast<float>(e.width), static_cast<float>(e.height), 0, 1};
        scissor = {0, 0, e.width, e.height};
        emit_viewport();
        emit_scissor();
        uint32_t buffers = 0;
        if (cmd.pass.clear_color) buffers |= virgl::kClearColor0;
        if (depth && cmd.pass.clear_depth) buffers |= virgl::kClearDepth;
        if (buffers) {
          stream.command(virgl::kClear, 0, 8);
          stream.put(buffers);
          stream.putf(cmd.pass.clear_color_value.x);
          stream.putf(cmd.pass.clear_color_value.y);
          stream.putf(cmd.pass.clear_color_value.z);
          stream.putf(cmd.pass.clear_color_value.w);
          const double d = cmd.pass.clear_depth_value;
          uint64_t bits;
          __builtin_memcpy(&bits, &d, 8);
          stream.put(static_cast<uint32_t>(bits));
          stream.put(static_cast<uint32_t>(bits >> 32));
          stream.put(0);  // stencil
        }
        pipeline = nullptr;
        in_pass = true;
        break;
      }
      case RecordedCommand::Type::EndRenderPass:
        in_pass = false;
        break;
      case RecordedCommand::Type::BindPipeline: {
        pipeline = static_cast<const VirglPipeline*>(cmd.pipeline);
        if (!pipeline) return false;
        stream.command(virgl::kBindObject, virgl::kObjectBlend, 1);
        stream.put(pipeline->blend());
        stream.command(virgl::kBindObject, virgl::kObjectDsa, 1);
        stream.put(pipeline->dsa());
        stream.command(virgl::kBindObject, virgl::kObjectRasterizer, 1);
        stream.put(pipeline->rasterizer());
        stream.command(virgl::kBindObject, virgl::kObjectVertexElements, 1);
        stream.put(pipeline->elements());
        stream.command(virgl::kBindShader, 0, 2);
        stream.put(pipeline->vertex_shader());
        stream.put(virgl::kShaderVertex);
        stream.command(virgl::kBindShader, 0, 2);
        stream.put(pipeline->fragment_shader());
        stream.put(virgl::kShaderFragment);
        constants_dirty = constants_dwords != 0;  // constants belong to the program: resend
        break;
      }
      case RecordedCommand::Type::BindVertexBuffer:
        vertex_buffer = static_cast<VirglBuffer*>(cmd.buffer);
        vertex_offset = cmd.offset;
        break;
      case RecordedCommand::Type::BindIndexBuffer:
        index_buffer = static_cast<VirglBuffer*>(cmd.buffer);
        index_offset = cmd.offset;
        index_type = cmd.index_type;
        break;
      case RecordedCommand::Type::BindTexture: return false;   // (no pipeline of this device samples a texture)
      case RecordedCommand::Type::PushConstants: {
        ConstByteView block;
        if (!pool.try_subview(cmd.constants_offset, cmd.constants_size, &block)) return false;
        for (uint32_t k = 0; k < block.size(); ++k) reinterpret_cast<uint8_t*>(constants)[k] = block[k];
        constants_dwords = cmd.constants_size / 4;
        constants_dirty = true;
        break;
      }
      case RecordedCommand::Type::SetViewport:
        viewport = cmd.viewport;
        emit_viewport();
        break;
      case RecordedCommand::Type::SetScissor: {
        if (!in_pass) return false;
        const Extent2D e = color->extent();
        const int64_t x0 = cmd.scissor.x > 0 ? cmd.scissor.x : 0, y0 = cmd.scissor.y > 0 ? cmd.scissor.y : 0;
        int64_t x1 = int64_t{cmd.scissor.x} + cmd.scissor.width, y1 = int64_t{cmd.scissor.y} + cmd.scissor.height;
        if (x1 > e.width) x1 = e.width;
        if (y1 > e.height) y1 = e.height;
        scissor = {static_cast<int32_t>(x0), static_cast<int32_t>(y0), x1 > x0 ? static_cast<uint32_t>(x1 - x0) : 0,
                   y1 > y0 ? static_cast<uint32_t>(y1 - y0) : 0};
        emit_scissor();
        break;
      }
      case RecordedCommand::Type::Draw:
      case RecordedCommand::Type::DrawIndexed: {
        if (!in_pass || !pipeline || !vertex_buffer) return false;
        const bool indexed = cmd.type == RecordedCommand::Type::DrawIndexed;
        if (indexed && !index_buffer) return false;
        if (constants_dirty) {
          for (uint32_t shader = 0; shader < 2; ++shader) {
            stream.command(virgl::kSetConstantBuffer, 0, 2 + constants_dwords);
            stream.put(shader);
            stream.put(0);
            for (uint32_t k = 0; k < constants_dwords; ++k) stream.put(constants[k]);
          }
          constants_dirty = false;
        }
        stream.command(virgl::kSetVertexBuffers, 0, 3);
        stream.put(pipeline->desc().vertex_stride);
        stream.put(static_cast<uint32_t>(vertex_offset));
        stream.put(vertex_buffer->id());
        if (indexed) {
          stream.command(virgl::kSetIndexBuffer, 0, 3);
          stream.put(index_buffer->id());
          stream.put(index_type == IndexType::Uint16 ? 2 : 4);
          stream.put(static_cast<uint32_t>(index_offset));
        }
        stream.command(virgl::kDrawVbo, 0, 12);
        stream.put(cmd.first);
        stream.put(cmd.count);
        stream.put(virgl::kPrimTriangles);
        stream.put(indexed ? 1 : 0);
        stream.put(1);  // instance count
        stream.put(static_cast<uint32_t>(cmd.vertex_offset));
        stream.put(0);  // start instance
        stream.put(0);  // primitive restart
        stream.put(0);  // restart index
        stream.put(0);  // min index
        stream.put(0xFFFFFFFFu);  // max index
        stream.put(0);  // count from stream output
        break;
      }
    }
    if (stream.overflow()) return false;
    if (stream.count() > Stream::kCapacity - 512 && !flush()) return false;  // leave room for the next command
  }
  return !in_pass && flush();
}

bool VirglDevice::present(IImage* image) {
  auto* img = static_cast<VirglImage*>(image);
  if (!img || !(img->usage() & kImagePresent)) return false;
  return gpu_.show_resource(img->id(), img->extent().width, img->extent().height);
}

}  // namespace graphics
