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

#include <initializer_list>
#include "Graphics/Graphics3D/Virgl/VirglDevice.hpp"
#include <new>
#include "Graphics/Graphics3D/Virgl/VirglProtocol.hpp"
#include "Utility/AllocatedView.hpp"
#include "Utility/ByteStream.hpp"

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

constexpr uint32_t kQueryPolls = 5000;   // how many empty commands to wait for the host's query results with

// --- Objects -------------------------------------------------------------------

class VirglBuffer final : public IBuffer {
 public:
  VirglBuffer(VirglDevice& device, uint64_t size, uint32_t usage, uint32_t extra_bind = 0) : device_(device) {
    hardware::VirtioGpu& gpu = device.gpu();
    memory_ = allocate_dma_view<uint8_t>(gpu.allocator(), size, 4096);
    if (memory_.empty()) return;
    memory_.fill(0);
    id_ = gpu.allocate_resource_id();
    uint32_t bind = extra_bind;
    if (usage & kBufferVertex) bind |= virgl::kBindVertexBuffer;
    if (usage & kBufferIndex) bind |= virgl::kBindIndexBuffer;
    if (usage & kBufferStorage) bind |= virgl::kBindShaderBuffer;
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
  // What a compute shader wrote lives on the host: pull it into the guest memory.
  bool read_back() override {
    TransferHost3d transfer{header(kCmdTransferFromHost3d), {0, 0, 0, static_cast<uint32_t>(memory_.size()), 1, 1}, 0, id_, 0, 0, 0};
    return device_.gpu().command(as_bytes(transfer));
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
    color_format_ = desc.format == Format::R32G32B32A32Float ? virgl::kFormatR32G32B32A32Float : desc.format == Format::R16G16B16A16Float ? virgl::kFormatR16G16B16A16Float : virgl::kFormatB8G8R8A8Unorm;
    const uint32_t depth_format = desc.stencil ? virgl::kFormatZ32FloatS8X24Uint : virgl::kFormatZ32Float;
    // A sampled image is stored top row first, the way write_pixels() gives it and the sampler reads it, so it does not take the Y_0_TOP flip of the images that are only
    // drawn into (and shown).
    const bool sampled = (desc.usage & kImageSampled) != 0;
    uint32_t bind = depth ? virgl::kBindDepthStencil : (virgl::kBindRenderTarget | virgl::kBindSamplerView);
    if (desc.usage & kImagePresent) bind |= virgl::kBindScanout;
    // A volume has its slices as the depth; an array or a cube its layers as the array size (a cube: six).
    const uint32_t layers = layer_count();
    const uint32_t target = desc.kind == TextureKind::Texture3D ? virgl::kTargetTexture3d : desc.kind == TextureKind::TextureCube ? virgl::kTargetTextureCube
                            : desc.kind == TextureKind::Texture2DArray ? virgl::kTargetTexture2dArray : virgl::kTargetTexture2d;
    ResourceCreate3d create{header(kCmdResourceCreate3d), id_, target,
                            depth ? depth_format : color_format_, bind,
                            desc.extent.width, desc.extent.height, desc.kind == TextureKind::Texture3D ? layers : 1u, desc.kind == TextureKind::Texture3D || desc.kind == TextureKind::Texture2D ? 1u : layers, 0, 0,
                            sampled ? 0u : kResourceFlagY0Top, 0};
    CtxResource ctx{header(kCmdCtxAttachResource), id_, 0};
    ok_ = gpu.command(as_bytes(create)) && gpu.command(as_bytes(ctx));
    accounted_ = uint64_t{desc.extent.width} * desc.extent.height * layer_count() * (depth ? 4u : bytes_per_pixel(desc.format));
    gpu.add_resource_bytes(accounted_);
    // The surface (a render-target view of the resource) is what the framebuffer state refers to.
    if (ok_ && (desc.usage & (kImageColorAttachment | kImageDepthAttachment))) {
      surface_ = device.new_handle();
      const uint32_t fmt = depth ? depth_format : color_format_;
      const uint32_t cmd[] = {virgl::header(virgl::kCreateObject, virgl::kObjectSurface, 5), surface_, id_, fmt, 0, 0};
      ok_ = device.send_commands(cmd, 6);
    }
    // The sampler view is what a texture unit is bound to: the whole resource, level 0, components in order.
    if (ok_ && sampled) {
      view_ = device.new_handle();
      // (the layers the view shows: all of an array's or a cube's; a volume's slices are not layers)
      const uint32_t last_layer = desc.kind == TextureKind::Texture2DArray || desc.kind == TextureKind::TextureCube ? layers - 1 : 0;
      const uint32_t cmd[] = {virgl::header(virgl::kCreateObject, virgl::kObjectSamplerView, 6), view_, id_, color_format_, last_layer << 16, 0, virgl::kSwizzleRgba};
      ok_ = device.send_commands(cmd, 7);
    }
  }
  ~VirglImage() {
    hardware::VirtioGpu& gpu = device_.gpu();
    if (view_) device_.destroy_object(view_);
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
  uint32_t view() const { return view_; }
  Extent2D extent() const override { return desc_.extent; }
  Format format() const override { return desc_.format; }
  uint32_t usage() const { return desc_.usage; }
  bool stencil() const { return desc_.stencil; }
  uint32_t layer_count() const override { return desc_.kind == TextureKind::Texture2D ? 1 : desc_.kind == TextureKind::TextureCube ? 6 : desc_.layers; }
  // A layer (a slice, a face) goes through the one-layer backing: the host copies from or into it at the layer's place in the resource.
  bool write_layer_pixels(uint32_t layer, ArrayView<const uint32_t> pixels) override {
    const uint64_t count = uint64_t{desc_.extent.width} * desc_.extent.height;
    if (desc_.kind == TextureKind::Texture2D || layer >= layer_count() || pixels.size() != count || !ensure_backing(count)) return false;
    const ArrayView<uint32_t> staged = backing_.cast<uint32_t>();
    for (uint64_t i = 0; i < count; ++i) staged[i] = pixels[i];
    TransferHost3d transfer{header(kCmdTransferToHost3d), {0, 0, layer, desc_.extent.width, desc_.extent.height, 1}, 0, id_, 0, desc_.extent.width * 4, 0};
    return device_.gpu().command(as_bytes(transfer));
  }
  bool read_layer_pixels(uint32_t layer, ArrayView<uint32_t> out) override {
    const uint64_t count = uint64_t{desc_.extent.width} * desc_.extent.height;
    if (desc_.kind == TextureKind::Texture2D || layer >= layer_count() || out.size() < count || !ensure_backing(count)) return false;
    TransferHost3d transfer{header(kCmdTransferFromHost3d), {0, 0, layer, desc_.extent.width, desc_.extent.height, 1}, 0, id_, 0, desc_.extent.width * 4, 0};
    if (!device_.gpu().command(as_bytes(transfer))) return false;
    const ArrayView<uint32_t> pixels = backing_.cast<uint32_t>();
    for (uint64_t i = 0; i < count; ++i) out[i] = pixels[i];
    return true;
  }
  // Stored with window y 0 at the top (drawn into and shown: the Y_0_TOP flag), or the way a texture is - row 0 first, window y 0 at the bottom of GL's window - which is what a
  // render target that is also sampled needs, so that the sampler reads it the way a texture is read (drawn upright by an unflipped viewport, see VirglPipeline).
  bool y0_top() const { return !(desc_.usage & kImageSampled); }

  bool read_pixels(ArrayView<uint32_t> out) override {
    const uint64_t count = uint64_t{desc_.extent.width} * desc_.extent.height;
    if (desc_.format == Format::D32Float || is_float_format(desc_.format) || out.size() < count) return false;
    hardware::VirtioGpu& gpu = device_.gpu();
    if (!ensure_backing(count)) return false;
    TransferHost3d transfer{header(kCmdTransferFromHost3d),
                            {0, 0, 0, desc_.extent.width, desc_.extent.height, 1}, 0, id_, 0,
                            desc_.extent.width * 4, 0};
    if (!gpu.command(as_bytes(transfer))) return false;
    const ArrayView<uint32_t> pixels = backing_.cast<uint32_t>();
    for (uint64_t i = 0; i < count; ++i) out[i] = pixels[i];
    return true;
  }

  bool read_pixels_float(ArrayView<Vec4> out) override {
    const uint64_t count = uint64_t{desc_.extent.width} * desc_.extent.height;
    if (!is_float_format(desc_.format) || out.size() < count || !ensure_backing(count)) return false;
    TransferHost3d transfer{header(kCmdTransferFromHost3d), {0, 0, 0, desc_.extent.width, desc_.extent.height, 1}, 0, id_, 0, desc_.extent.width * bytes_per_pixel(desc_.format), 0};
    if (!device_.gpu().command(as_bytes(transfer))) return false;
    ByteReader in{ConstByteView(backing_)};
    for (uint64_t i = 0; i < count; ++i) {
      if (desc_.format == Format::R16G16B16A16Float) {
        const float r = half_to_float(in.le16()), g = half_to_float(in.le16()), b = half_to_float(in.le16()), a = half_to_float(in.le16());
        out[i] = {r, g, b, a};
      } else {
        const float r = __builtin_bit_cast(float, in.le32()), g = __builtin_bit_cast(float, in.le32()), b = __builtin_bit_cast(float, in.le32()), a = __builtin_bit_cast(float, in.le32());
        out[i] = {r, g, b, a};
      }
    }
    return in.ok();
  }

  bool write_pixels_float(ArrayView<const Vec4> pixels) override {
    const uint64_t count = uint64_t{desc_.extent.width} * desc_.extent.height;
    if (!is_float_format(desc_.format) || !(desc_.usage & kImageSampled) || pixels.size() != count || !ensure_backing(count)) return false;
    ByteWriter out{backing_};
    for (const Vec4& c : pixels) {
      if (desc_.format == Format::R16G16B16A16Float) {
        out.le16(float_to_half(c.x)), out.le16(float_to_half(c.y)), out.le16(float_to_half(c.z)), out.le16(float_to_half(c.w));
      } else {
        out.le32(__builtin_bit_cast(uint32_t, c.x)), out.le32(__builtin_bit_cast(uint32_t, c.y)), out.le32(__builtin_bit_cast(uint32_t, c.z)), out.le32(__builtin_bit_cast(uint32_t, c.w));
      }
    }
    if (!out.ok()) return false;
    TransferHost3d transfer{header(kCmdTransferToHost3d), {0, 0, 0, desc_.extent.width, desc_.extent.height, 1}, 0, id_, 0, desc_.extent.width * bytes_per_pixel(desc_.format), 0};
    return device_.gpu().command(as_bytes(transfer));
  }

  // Only a texture takes pixels: the host copies them from guest memory (the backing) into the resource.
  bool write_pixels(ArrayView<const uint32_t> pixels) override {
    const uint64_t count = uint64_t{desc_.extent.width} * desc_.extent.height;
    if (!(desc_.usage & kImageSampled) || desc_.format == Format::D32Float || is_float_format(desc_.format) || pixels.size() != count) return false;
    if (!ensure_backing(count)) return false;
    const ArrayView<uint32_t> staged = backing_.cast<uint32_t>();
    for (uint64_t i = 0; i < count; ++i) staged[i] = pixels[i];
    TransferHost3d transfer{header(kCmdTransferToHost3d), {0, 0, 0, desc_.extent.width, desc_.extent.height, 1}, 0, id_, 0, desc_.extent.width * 4, 0};
    return device_.gpu().command(as_bytes(transfer));
  }

 private:
  // Guest memory for the host to copy from or into, attached on first use.
  bool ensure_backing(uint64_t pixel_count) {
    if (!backing_.empty()) return true;
    hardware::VirtioGpu& gpu = device_.gpu();
    const uint64_t bytes = pixel_count * bytes_per_pixel(desc_.format);
    backing_ = allocate_dma_view<uint8_t>(gpu.allocator(), bytes, 4096);
    if (backing_.empty()) return false;
    ResourceAttachBacking attach{header(kCmdResourceAttachBacking), id_, 1,
                                 {reinterpret_cast<uint64_t>(backing_.data()), static_cast<uint32_t>(bytes), 0}};
    if (!gpu.command(as_bytes(attach))) {
      release_view(gpu.allocator(), backing_);
      return false;
    }
    return true;
  }

  VirglDevice& device_;
  ImageDesc desc_;
  uint32_t id_ = 0, surface_ = 0, view_ = 0, color_format_ = 0;
  uint64_t accounted_ = 0;   // what the GPU's video memory figure counts for this image
  ByteView backing_;
  bool ok_ = false;
};

class VirglShaderModule final : public IShaderModule {
 public:
  VirglShaderModule(VirglDevice& device, const ShaderModuleDesc& desc) : stage_(desc.stage) {
    for (uint32_t k = 0; k < 3; ++k) thread_group_[k] = desc.thread_group[k];
    handle_ = device.new_handle();
    const uint32_t length = string_length(desc.tgsi) + 1;  // with the terminating NUL
    const uint32_t text_dwords = (length + 3) / 4;
    const uint32_t total = 6 + text_dwords;
    hardware::VirtioGpu& gpu = device.gpu();
    uint32_t* cmd = static_cast<uint32_t*>(gpu.allocator().allocate(total * 4));
    if (!cmd) return;
    cmd[0] = virgl::header(virgl::kCreateObject, virgl::kObjectShader, 5 + text_dwords);
    cmd[1] = handle_;
    cmd[2] = desc.stage == ShaderStage::Vertex ? virgl::kShaderVertex : desc.stage == ShaderStage::Compute ? virgl::kShaderCompute : virgl::kShaderFragment;
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
  uint32_t thread_group(uint32_t axis) const { return thread_group_[axis]; }

 private:
  uint32_t thread_group_[3] = {1, 1, 1};
  ShaderStage stage_;
  uint32_t handle_ = 0;
  bool ok_ = false;
};

// Both kinds of pipeline, so that destroy_pipeline can tell them apart.
class VirglPipelineBase : public IPipeline {
 public:
  bool compute() const { return compute_; }

 protected:
  explicit VirglPipelineBase(bool compute) : compute_(compute) {}
  ~VirglPipelineBase() = default;

 private:
  bool compute_;
};

class VirglComputePipeline final : public VirglPipelineBase {
 public:
  explicit VirglComputePipeline(const VirglShaderModule& shader) : VirglPipelineBase(true), shader_(shader.handle()) {
    for (uint32_t k = 0; k < 3; ++k) group_[k] = shader.thread_group(k);
  }
  uint32_t shader() const { return shader_; }
  uint32_t group(uint32_t axis) const { return group_[axis]; }

 private:
  uint32_t shader_;
  uint32_t group_[3];
};

class VirglPipeline final : public VirglPipelineBase {
 public:
  VirglPipeline(VirglDevice& device, const GraphicsPipelineDesc& desc)
      : VirglPipelineBase(false), device_(device), desc_(desc), vs_(static_cast<const VirglShaderModule*>(desc.vertex_shader)->handle()),
        fs_(static_cast<const VirglShaderModule*>(desc.fragment_shader)->handle()) {
    blend_ = device.new_handle();
    dsa_ = device.new_handle();
    rasterizer_ = device.new_handle();
    elements_ = device.new_handle();
    uint32_t cmd[64];
    uint32_t n = 0;

    // Blend state: source-over blending (alpha_blend) or the pipeline's own state (kFeatureBlendState); with independent blending each target has a state of its own, else target 0's
    // serves them all.
    auto render_target_blend = [](const BlendState& bs) {
      // (min and max take no factors: Gallium wants ONE for them)
      const bool color_minmax = bs.color_op == BlendOp::Min || bs.color_op == BlendOp::Max, alpha_minmax = bs.alpha_op == BlendOp::Min || bs.alpha_op == BlendOp::Max;
      const uint32_t color_src = color_minmax ? virgl::kBlendFactorOne : virgl::kBlendFactors[static_cast<uint32_t>(bs.src_color)];
      const uint32_t color_dst = color_minmax ? virgl::kBlendFactorOne : virgl::kBlendFactors[static_cast<uint32_t>(bs.dst_color)];
      const uint32_t alpha_src = alpha_minmax ? virgl::kBlendFactorOne : virgl::kBlendFactors[static_cast<uint32_t>(bs.src_alpha)];
      const uint32_t alpha_dst = alpha_minmax ? virgl::kBlendFactorOne : virgl::kBlendFactors[static_cast<uint32_t>(bs.dst_alpha)];
      return (bs.enable ? 1u : 0u) | (static_cast<uint32_t>(bs.color_op) << 1) | (color_src << 4) | (color_dst << 9) | (static_cast<uint32_t>(bs.alpha_op) << 14) | (alpha_src << 17) |
             (alpha_dst << 22) | (static_cast<uint32_t>(bs.write_mask & 0xF) << 27);
    };
    uint32_t blend_rt = 0xFu << 27;
    if (desc.blend.custom) {
      blend_rt = render_target_blend(desc.blend);
    } else if (desc.alpha_blend) {
      blend_rt = 1u | (virgl::kBlendAdd << 1) | (virgl::kBlendFactorSrcAlpha << 4) | (virgl::kBlendFactorInvSrcAlpha << 9) | (virgl::kBlendAdd << 14) | (virgl::kBlendFactorOne << 17) |
                 (virgl::kBlendFactorInvSrcAlpha << 22) | (0xFu << 27);
    }
    cmd[n++] = virgl::header(virgl::kCreateObject, virgl::kObjectBlend, 11);
    cmd[n++] = blend_;
    cmd[n++] = desc.independent_blend ? 1u : 0u;  // s0: independent blend enable (no logic op, dither...)
    cmd[n++] = 0;  // s1
    cmd[n++] = blend_rt;
    for (uint32_t i = 1; i < 8; ++i) cmd[n++] = desc.independent_blend && i < kMaxColorTargets ? (desc.target_blend[i - 1].custom ? render_target_blend(desc.target_blend[i - 1]) : 0xFu << 27) : 0u;

    // Depth and stencil (the same stencil state for both faces).
    const uint32_t depth_func = desc.depth_test ? static_cast<uint32_t>(desc.depth_compare) : virgl::kFuncAlways;   // (CompareOp is numbered as PIPE_FUNC)
    const bool depth_on = desc.depth_test || desc.depth_write;
    const StencilState& st = desc.stencil;
    const uint32_t stencil_state = !st.enable ? 0u
                                              : 1u | (static_cast<uint32_t>(st.compare) << 1) | (virgl::kStencilOps[static_cast<uint32_t>(st.fail_op)] << 4) |
                                                    (virgl::kStencilOps[static_cast<uint32_t>(st.pass_op)] << 7) | (virgl::kStencilOps[static_cast<uint32_t>(st.depth_fail_op)] << 10) |
                                                    (uint32_t{st.compare_mask} << 13) | (uint32_t{st.write_mask} << 21);
    stencil_reference_ = st.enable ? st.reference : 0;
    cmd[n++] = virgl::header(virgl::kCreateObject, virgl::kObjectDsa, 5);
    cmd[n++] = dsa_;
    cmd[n++] = (depth_on ? 1u : 0u) | ((desc.depth_write ? 1u : 0u) << 1) | (depth_func << 2);
    cmd[n++] = stencil_state;
    cmd[n++] = stencil_state;
    cmd[n++] = 0;   // alpha reference

    // Rasterizer: depth clip with a 0..1 clip volume (Vulkan), scissor on, GL pixel conventions.
    // Y_0_TOP images make window space match Vulkan's, which flips what GL calls counter-clockwise.
    const uint32_t cull = desc.cull_mode == CullMode::None ? virgl::kFaceNone
                          : desc.cull_mode == CullMode::Back ? virgl::kFaceBack : virgl::kFaceFront;
    const bool front_ccw = desc.front_face == FrontFace::CounterClockwise;
    // (the winding is the same for both kinds of image: the viewport flip and Y_0_TOP, or neither, both put the picture's top row at the same place for the rasterizer)
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
      cmd[n++] = desc.attributes[i].per_instance ? 1 : 0;   // instance divisor
      cmd[n++] = desc.attributes[i].per_instance ? 1 : 0;   // vertex buffer index (the divisor belongs to a buffer binding on the host: per-instance attributes get a binding of their own, of the same buffer)
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
  bool has_per_instance() const {
    for (uint32_t i = 0; i < desc_.attribute_count; ++i)
      if (desc_.attributes[i].per_instance) return true;
    return false;
  }
  uint8_t stencil_reference() const { return stencil_reference_; }
  bool stencil_enabled() const { return desc_.stencil.enable; }
  uint32_t vertex_shader() const { return vs_; }
  uint32_t fragment_shader() const { return fs_; }

 private:
  VirglDevice& device_;
  GraphicsPipelineDesc desc_;
  uint32_t vs_, fs_;
  uint8_t stencil_reference_ = 0;
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

VirglDevice::~VirglDevice() {
  for (uint32_t handle : sampler_states_) destroy_object(handle);
}

uint32_t VirglDevice::sampler_state(const SamplerDesc& sampler) {
  const uint32_t key = (sampler.filter == Filter::Linear ? 1u : 0u) | (sampler.address_u == AddressMode::ClampToEdge ? 2u : 0u) | (sampler.address_v == AddressMode::ClampToEdge ? 4u : 0u);
  if (sampler_states_[key]) return sampler_states_[key];
  const uint32_t handle = new_handle();
  const uint32_t wrap_s = sampler.address_u == AddressMode::Repeat ? virgl::kWrapRepeat : virgl::kWrapClampToEdge;
  const uint32_t wrap_t = sampler.address_v == AddressMode::Repeat ? virgl::kWrapRepeat : virgl::kWrapClampToEdge;
  const uint32_t filter = sampler.filter == Filter::Linear ? virgl::kFilterLinear : virgl::kFilterNearest;
  // No mipmaps: the minification filter is the magnification one and no level is chosen between.
  const uint32_t s0 = wrap_s | (wrap_t << 3) | (virgl::kWrapClampToEdge << 6) /* r: the slices of a volume are clamped */ | (filter << 9) | (virgl::kMipFilterNone << 11) | (filter << 13);
  const uint32_t cmd[] = {virgl::header(virgl::kCreateObject, virgl::kObjectSamplerState, 9), handle, s0, 0, 0, 0, 0, 0, 0, 0};
  if (!send_commands(cmd, 10)) return 0;
  sampler_states_[key] = handle;
  return handle;
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
  if (desc.mip_levels != 1) return nullptr;   // (no mipmaps either)
  if (desc.kind != TextureKind::Texture2D) {
    // (a volume, array or cube is only sampled: 8-bit colour, one level)
    const uint32_t layers = desc.kind == TextureKind::TextureCube ? 6 : desc.layers;
    if (desc.usage != kImageSampled || desc.format != Format::B8G8R8A8Unorm || layers == 0 || layers > kMaxTextureLayers || (desc.kind == TextureKind::TextureCube && desc.extent.width != desc.extent.height)) return nullptr;
  } else if (desc.layers != 1) {
    return nullptr;
  }
  // A texture is not drawn into: the two kinds of image are stored in opposite row order (see VirglImage).
  if ((desc.usage & kImageSampled) && (desc.usage & (kImageDepthAttachment | kImagePresent))) return nullptr;
  if ((desc.usage & kImageSampled) && desc.format != Format::B8G8R8A8Unorm && !is_float_format(desc.format)) return nullptr;
  if (!desc.extent.width || !desc.extent.height || desc.format == Format::Undefined) return nullptr;
  if (desc.format == Format::R8G8B8A8Unorm) return nullptr;  // only BGRA color images for now
  if (desc.stencil && desc.format != Format::D32Float) return nullptr;
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
  if (desc.texture_count > kMaxTextureUnits || desc.fragment_shader == nullptr) return nullptr;
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
void VirglDevice::destroy_pipeline(IPipeline* pipeline) {
  if (static_cast<VirglPipelineBase*>(pipeline)->compute()) destroy(gpu_.allocator(), static_cast<VirglComputePipeline*>(pipeline));
  else destroy(gpu_.allocator(), static_cast<VirglPipeline*>(pipeline));
}

IPipeline* VirglDevice::create_compute_pipeline(const IShaderModule* shader) {
  if (!shader || shader->stage() != ShaderStage::Compute) return nullptr;
  return make<VirglComputePipeline>(gpu_.allocator(), *static_cast<const VirglShaderModule*>(shader));
}

ICommandBuffer* VirglDevice::create_command_buffer() { return make<CommandRecorder>(gpu_.allocator(), gpu_.allocator()); }
void VirglDevice::destroy_command_buffer(ICommandBuffer* commands) {
  destroy(gpu_.allocator(), static_cast<CommandRecorder*>(commands));
}

bool VirglDevice::submit(ICommandBuffer* commands) {
  if (!commands) return false;
  CommandRecorder& recorder = *static_cast<CommandRecorder*>(commands);
  Stream stream(gpu_.allocator());
  if (!stream.valid()) return false;

  // Occlusion queries: one host query object per slot, each with a buffer of its own for its result (16 bytes: state, size, the count) that the host fills in when asked for
  // it after the commands have run. (The host takes no offset into a shared buffer: two queries on one buffer overwrite each other.)
  struct Queries {
    explicit Queries(VirglDevice& d) : device(d) {}
    ~Queries() {
      for (uint32_t handle : objects)
        if (handle) device.destroy_object(handle);
      for (VirglBuffer* buffer : results)
        if (buffer) destroy(device.gpu().allocator(), buffer);
    }
    VirglDevice& device;
    VirglBuffer* results[kMaxOcclusionQueries] = {};
    uint32_t objects[kMaxOcclusionQueries] = {};
    bool used[kMaxOcclusionQueries] = {};
    uint32_t open = kMaxOcclusionQueries;   // the slot between begin and end
  } queries(*this);

  const VirglPipeline* pipeline = nullptr;
  VirglImage* color = nullptr;
  VirglImage* extra_colors[kMaxColorTargets - 1] = {};
  uint32_t target_count = 1;
  VirglImage* depth = nullptr;
  VirglBuffer* vertex_buffer = nullptr;
  uint64_t vertex_offset = 0;
  const VirglImage* textures[kMaxTextureUnits] = {};
  SamplerDesc samplers[kMaxTextureUnits];
  bool textures_dirty = false;
  VirglBuffer* index_buffer = nullptr;
  uint64_t index_offset = 0;
  IndexType index_type = IndexType::Uint16;
  Viewport viewport;
  Rect2D scissor;
  uint32_t constants[kMaxPushConstantBytes / 4] = {};
  uint32_t constants_dwords = 0;
  bool constants_dirty = false;
  bool in_pass = false;
  PrimitiveTopology topology = PrimitiveTopology::TriangleList;
  const VirglComputePipeline* compute = nullptr;
  struct StorageBinding {
    VirglBuffer* buffer = nullptr;
    uint64_t offset = 0, size = 0;
  } storage[kMaxStorageBuffers];
  bool storage_dirty = false, compute_constants_dirty = false;
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
    stream.putf((color && !color->y0_top() ? 1.0f : -1.0f) * viewport.height * 0.5f);  // GL's +Y is up: flip to Vulkan's +Y down (an upright image has its row 0 at window y 0 already)
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
    const bool upright = color && !color->y0_top();
    const uint32_t y0 = upright ? scissor.y : height - (scissor.y + scissor.height), y1 = upright ? scissor.y + scissor.height : height - scissor.y;
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
        target_count = 1;
        for (uint32_t n = 0; n + 1 < kMaxColorTargets; ++n) {
          extra_colors[n] = static_cast<VirglImage*>(cmd.pass.extra_colors[n]);
          if (!extra_colors[n]) {
            for (uint32_t k = n + 1; k + 1 < kMaxColorTargets; ++k)
              if (cmd.pass.extra_colors[k]) return false;   // (a gap)
            break;
          }
          VirglImage* extra = extra_colors[n];
          // (one set of window-space conventions for the whole framebuffer)
          if (!extra->surface() || extra->format() == Format::D32Float || extra == color || extra == depth || extra->y0_top() != color->y0_top() ||
              extra->extent().width != e.width || extra->extent().height != e.height)
            return false;
          for (uint32_t k = 0; k < n; ++k)
            if (extra_colors[k] == extra) return false;
          target_count = n + 2;
        }
        for (uint32_t n = target_count - 1; n + 1 < kMaxColorTargets; ++n) extra_colors[n] = nullptr;
        stream.command(virgl::kSetFramebufferState, 0, 2 + target_count);
        stream.put(target_count);
        stream.put(depth ? depth->surface() : 0);
        stream.put(color->surface());
        for (uint32_t n = 0; n + 1 < target_count; ++n) stream.put(extra_colors[n]->surface());
        viewport = {0, 0, static_cast<float>(e.width), static_cast<float>(e.height), 0, 1};
        scissor = {0, 0, e.width, e.height};
        emit_viewport();
        emit_scissor();
        uint32_t buffers = 0;
        if (cmd.pass.clear_color)
          for (uint32_t n = 0; n < target_count; ++n) buffers |= virgl::kClearColor0 << n;
        if (depth && cmd.pass.clear_depth) buffers |= virgl::kClearDepth;
        if (depth && depth->stencil() && cmd.pass.clear_stencil) buffers |= virgl::kClearStencil;
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
          stream.put(cmd.pass.clear_stencil_value);
        }
        pipeline = nullptr;
        in_pass = true;
        break;
      }
      case RecordedCommand::Type::SetTopology:
        topology = cmd.topology;
        break;
      case RecordedCommand::Type::EndRenderPass:
        if (queries.open != kMaxOcclusionQueries) return false;
        in_pass = false;
        break;
      case RecordedCommand::Type::BindComputePipeline: {
        compute = static_cast<const VirglComputePipeline*>(cmd.pipeline);
        if (in_pass || !compute) return false;
        stream.command(virgl::kBindShader, 0, 2);
        stream.put(compute->shader());
        stream.put(virgl::kShaderCompute);
        compute_constants_dirty = constants_dwords != 0;
        break;
      }
      case RecordedCommand::Type::BindStorageBuffer:
        if (in_pass || cmd.unit >= kMaxStorageBuffers || !cmd.buffer) return false;
        storage[cmd.unit] = {static_cast<VirglBuffer*>(cmd.buffer), cmd.offset, cmd.count};
        storage_dirty = true;
        break;
      case RecordedCommand::Type::Dispatch: {
        if (in_pass || !compute) return false;
        uint32_t slots = 0;   // (bound slots are a prefix: the host takes them from slot 0 up to the last)
        for (uint32_t k = 0; k < kMaxStorageBuffers; ++k)
          if (storage[k].buffer) slots = k + 1;
        if (storage_dirty && slots) {
          stream.command(virgl::kSetShaderBuffers, 0, 2 + 3 * slots);
          stream.put(virgl::kShaderCompute);
          stream.put(0);
          for (uint32_t k = 0; k < slots; ++k) {
            stream.put(static_cast<uint32_t>(storage[k].offset));
            stream.put(static_cast<uint32_t>(storage[k].size));
            stream.put(storage[k].buffer ? storage[k].buffer->id() : 0);
          }
          storage_dirty = false;
        }
        if (compute_constants_dirty) {
          stream.command(virgl::kSetConstantBuffer, 0, 2 + constants_dwords);
          stream.put(virgl::kShaderCompute);
          stream.put(0);
          for (uint32_t k = 0; k < constants_dwords; ++k) stream.put(constants[k]);
          compute_constants_dirty = false;
        }
        stream.command(virgl::kLaunchGrid, 0, 8);
        stream.put(compute->group(0));
        stream.put(compute->group(1));
        stream.put(compute->group(2));
        stream.put(cmd.count);
        stream.put(cmd.first);
        stream.put(cmd.instance_count);
        stream.put(0);   // not indirect
        stream.put(0);
        stream.command(virgl::kMemoryBarrier, 0, 1);
        stream.put(0xFFFFFFFFu);
        break;
      }
      case RecordedCommand::Type::BeginQuery: {
        if (!in_pass || queries.open != kMaxOcclusionQueries || cmd.unit >= kMaxOcclusionQueries || queries.used[cmd.unit]) return false;
        queries.results[cmd.unit] = make<VirglBuffer>(gpu_.allocator(), *this, 16, 0, virgl::kBindCustom);
        if (!queries.results[cmd.unit] || !queries.results[cmd.unit]->valid()) return false;
        const uint32_t handle = new_handle();
        stream.command(virgl::kCreateObject, virgl::kObjectQuery, 4);
        stream.put(handle);
        stream.put(virgl::kQueryOcclusionCounter);
        stream.put(0);
        stream.put(queries.results[cmd.unit]->id());
        stream.command(virgl::kBeginQuery, 0, 1);
        stream.put(handle);
        queries.objects[cmd.unit] = handle;
        queries.used[cmd.unit] = true;
        queries.open = cmd.unit;
        break;
      }
      case RecordedCommand::Type::EndQuery:
        if (queries.open == kMaxOcclusionQueries) return false;
        stream.command(virgl::kEndQuery, 0, 1);
        stream.put(queries.objects[queries.open]);
        queries.open = kMaxOcclusionQueries;
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
        if (pipeline->stencil_enabled()) {
          stream.command(virgl::kSetStencilRef, 0, 1);
          stream.put(pipeline->stencil_reference() | (uint32_t{pipeline->stencil_reference()} << 8));
        }
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
      case RecordedCommand::Type::BindTexture: {
        const VirglImage* image = static_cast<const VirglImage*>(cmd.image);
        if (cmd.unit >= kMaxTextureUnits || !image || !image->view()) return false;   // (only an image created with kImageSampled has a view)
        textures[cmd.unit] = image;
        samplers[cmd.unit] = cmd.sampler;
        textures_dirty = true;
        break;
      }
      case RecordedCommand::Type::PushConstants: {
        ConstByteView block;
        if (!pool.try_subview(cmd.constants_offset, cmd.constants_size, &block)) return false;
        for (uint32_t k = 0; k < block.size(); ++k) reinterpret_cast<uint8_t*>(constants)[k] = block[k];
        constants_dwords = cmd.constants_size / 4;
        constants_dirty = true;
        compute_constants_dirty = true;
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
        if (!in_pass || !pipeline || !vertex_buffer || pipeline->desc().color_target_count > target_count) return false;
        const bool indexed = cmd.type == RecordedCommand::Type::DrawIndexed;
        if (indexed && !index_buffer) return false;
        if (const uint32_t unit_count = pipeline->desc().texture_count) {
          for (uint32_t unit = 0; unit < unit_count; ++unit)
            if (!textures[unit] || textures[unit] == color || textures[unit] == depth) return false;   // unbound, or the image being drawn into
          for (uint32_t unit = 0; unit < unit_count; ++unit)
            for (const VirglImage* extra : extra_colors)
              if (textures[unit] == extra) return false;
          if (textures_dirty) {
            stream.command(virgl::kSetSamplerViews, 0, 2 + unit_count);
            stream.put(virgl::kShaderFragment);
            stream.put(0);
            for (uint32_t unit = 0; unit < unit_count; ++unit) stream.put(textures[unit]->view());
            uint32_t states[kMaxTextureUnits];
            for (uint32_t unit = 0; unit < unit_count; ++unit) {
              states[unit] = sampler_state(samplers[unit]);   // (created straight away, so it exists before the stream that uses it)
              if (!states[unit]) return false;
            }
            stream.command(virgl::kBindSamplerStates, 0, 2 + unit_count);
            stream.put(virgl::kShaderFragment);
            stream.put(0);
            for (uint32_t unit = 0; unit < unit_count; ++unit) stream.put(states[unit]);
            textures_dirty = false;
          }
        }
        if (constants_dirty) {
          for (uint32_t shader = 0; shader < 2; ++shader) {
            stream.command(virgl::kSetConstantBuffer, 0, 2 + constants_dwords);
            stream.put(shader);
            stream.put(0);
            for (uint32_t k = 0; k < constants_dwords; ++k) stream.put(constants[k]);
          }
          constants_dirty = false;
        }
        const bool instanced_attributes = pipeline->has_per_instance();
        stream.command(virgl::kSetVertexBuffers, 0, instanced_attributes ? 6 : 3);
        for (uint32_t binding = 0; binding < (instanced_attributes ? 2u : 1u); ++binding) {
          stream.put(pipeline->desc().vertex_stride);
          stream.put(static_cast<uint32_t>(vertex_offset));
          stream.put(vertex_buffer->id());
        }
        if (indexed) {
          stream.command(virgl::kSetIndexBuffer, 0, 3);
          stream.put(index_buffer->id());
          stream.put(index_type == IndexType::Uint16 ? 2 : 4);
          stream.put(static_cast<uint32_t>(index_offset));
        }
        stream.command(virgl::kDrawVbo, 0, 12);
        stream.put(cmd.first);
        stream.put(cmd.count);
        stream.put(topology == PrimitiveTopology::LineList ? virgl::kPrimLines : topology == PrimitiveTopology::PointList ? virgl::kPrimPoints : virgl::kPrimTriangles);
        stream.put(indexed ? 1 : 0);
        stream.put(cmd.instance_count);
        stream.put(static_cast<uint32_t>(cmd.vertex_offset));
        stream.put(cmd.first_instance);
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
  if (in_pass || !flush()) return false;
  // Ask the host for each query's count (waiting for the draws), then read what it wrote into the buffer.
  bool asked = false;
  for (uint32_t slot = 0; slot < kMaxOcclusionQueries; ++slot) {
    if (!queries.used[slot]) continue;
    stream.command(virgl::kGetQueryResult, 0, 2);
    stream.put(queries.objects[slot]);
    stream.put(1);
    asked = true;
  }
  if (asked && !flush()) return false;
  // (the host writes each result - state, size, count - straight into the buffer's guest memory when its GL says the query is complete. It checks as it polls, which it does while
  // commands with fences are coming in: so keep it busy with empty ones until every state reads DONE. Pulling a buffer from the host would overwrite what it wrote.)
  for (uint32_t attempt = 0; asked; ++attempt) {
    bool pending = false;
    for (uint32_t slot = 0; slot < kMaxOcclusionQueries; ++slot) {
      if (!queries.used[slot]) continue;
      ByteReader in(queries.results[slot]->map());
      if (in.le32() != virgl::kQueryStateDone) pending = true;
    }
    if (!pending) break;
    const uint32_t nop = 0;
    if (attempt > kQueryPolls || !send_commands(&nop, 1)) return false;
  }
  for (uint32_t slot = 0; slot < kMaxOcclusionQueries; ++slot) {
    if (!queries.used[slot]) continue;
    ByteReader in(queries.results[slot]->map());
    in.le32();   // (the state, DONE)
    in.le32();   // (the size of the result: 4 for a sample count, 8 for a time)
    const uint64_t low = in.le32();
    const uint64_t high = in.le32();
    if (!in.ok()) return false;
    recorder.set_occlusion_result(slot, low | (high << 32));
  }
  return true;
}

bool VirglDevice::present(IImage* image) {
  auto* img = static_cast<VirglImage*>(image);
  if (!img || !(img->usage() & kImagePresent)) return false;
  return gpu_.show_resource(img->id(), img->extent().width, img->extent().height);
}

}  // namespace graphics
