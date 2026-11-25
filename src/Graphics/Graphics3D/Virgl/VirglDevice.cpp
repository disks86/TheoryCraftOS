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
#include "Graphics/WidePixels.hpp"
#include "Graphics/BlockCompression.hpp"
#include <new>
#include "Graphics/Graphics3D/Virgl/VirglProtocol.hpp"
#include "Utility/AllocatedView.hpp"
#include "Utility/ByteStream.hpp"
#include "Utility/ByteBuffer.hpp"

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

static_assert(sizeof virgl::kBlendFactors / sizeof virgl::kBlendFactors[0] == kBlendFactorCount, "a Gallium factor for every BlendFactor");
static_assert(sizeof virgl::kLogicOps / sizeof virgl::kLogicOps[0] == kLogicOpCount, "a Gallium logic operation for every LogicOp");

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

constexpr uint32_t kFillQueries = 16;   // the device's own queries in a submit (how far stream output buffers got)
constexpr uint32_t kQuerySlots = kMaxOcclusionQueries + kFillQueries;
constexpr uint32_t kQueryPolls = 5000;   // how many empty commands to wait for the host's query results with

// --- Objects -------------------------------------------------------------------

class VirglBuffer final : public IBuffer {
 public:
  VirglBuffer(VirglDevice& device, uint64_t size, uint32_t usage, uint32_t extra_bind = 0) : device_(device) {
    hardware::VirtioGpu& gpu = device.gpu();
    memory_ = allocate_dma_view<uint8_t>(gpu.allocator(), size, 4096);
    if (memory_.empty()) return;
    memory_.fill(0);
    bind_ = extra_bind;
    if (usage & kBufferVertex) bind_ |= virgl::kBindVertexBuffer;
    if (usage & kBufferIndex) bind_ |= virgl::kBindIndexBuffer;
    if (usage & kBufferStorage) bind_ |= virgl::kBindShaderBuffer;
    if (usage & kBufferStreamOutput) bind_ |= virgl::kBindStreamOutput;
    // The host takes a buffer with exactly one use (anything else is "Invalid texture bind flags"), but the use only picks the GL target the buffer object is first bound
    // to: any buffer object can then be bound as vertex, index, storage or stream output buffer. A buffer of several uses (every Direct3D 12 buffer is one) is made as a
    // vertex buffer, or as the first use it has.
    if (bind_ & (bind_ - 1)) bind_ = (bind_ & virgl::kBindVertexBuffer) ? virgl::kBindVertexBuffer : bind_ & (~bind_ + 1);
    id_ = gpu.allocate_resource_id();
    ok_ = make_resource(id_, /*fence=*/true);
    gpu.add_resource_bytes(size);
    accounted_ = size;
  }
  ~VirglBuffer() {
    hardware::VirtioGpu& gpu = device_.gpu();
    if (id_) drop_resource(id_, /*fence=*/true);
    gpu.remove_resource_bytes(accounted_);
    release_view(gpu.allocator(), memory_);
  }
  bool valid() const { return ok_; }
  uint32_t id() const { return id_; }
  uint64_t size() const override { return memory_.size(); }
  ByteView map() override { return memory_; }
  // The host writes an upload into the buffer straight away, without waiting for the GPU (virglrenderer maps it unsynchronized: keeping work that still reads
  // the old contents apart is the guest's job). So a buffer that a submit the GPU may not have finished draws from is not written over: its contents go to a
  // new host resource, which the next submits use, and the old one is let go - the host keeps it until the work that reads it is done. Without that a renderer
  // that fills a buffer, submits, fills it again and submits again (the desktop's, around a picture it shows from the GPU) has its first draws read the
  // second contents whenever the GPU is behind.
  void unmap() override {
    if (ok_ && device_.in_flight(last_use())) replace_busy_resource();
    TransferHost3d transfer{header(kCmdTransferToHost3d), {0, 0, 0, static_cast<uint32_t>(memory_.size()), 1, 1}, 0,
                            id_, 0, 0, 0};
    device_.gpu().command(as_bytes(transfer), /*fence=*/false);
  }
  // What a compute shader wrote lives on the host: pull it into the guest memory.
  bool read_back() override {
    TransferHost3d transfer{header(kCmdTransferFromHost3d), {0, 0, 0, static_cast<uint32_t>(memory_.size()), 1, 1}, 0, id_, 0, 0, 0};
    return device_.gpu().command(as_bytes(transfer));
  }
  // A submit was given to the host with commands that use this buffer: its number (VirtioGpu::work_sent). Called by the submit, from any thread.
  void note_use(uint64_t work) {
    uint64_t seen = __atomic_load_n(&last_use_, __ATOMIC_RELAXED);
    while (work > seen && !__atomic_compare_exchange_n(&last_use_, &seen, work, true, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {}
  }
  uint64_t last_use() const { return __atomic_load_n(&last_use_, __ATOMIC_ACQUIRE); }

 private:
  // A host resource for the buffer (`id`), backed by our memory and known to the context.
  bool make_resource(uint32_t id, bool fence) {
    hardware::VirtioGpu& gpu = device_.gpu();
    const uint32_t size = static_cast<uint32_t>(memory_.size());
    ResourceCreate3d create{header(kCmdResourceCreate3d), id, virgl::kTargetBuffer, virgl::kFormatR8Unorm, bind_, size, 1, 1, 1, 0, 0, 0, 0};
    ResourceAttachBacking attach{header(kCmdResourceAttachBacking), id, 1, {reinterpret_cast<uint64_t>(memory_.data()), size, 0}};
    CtxResource ctx{header(kCmdCtxAttachResource), id, 0};
    return gpu.command(as_bytes(create), fence) && gpu.command(as_bytes(attach), fence) && gpu.command(as_bytes(ctx), fence);
  }
  void drop_resource(uint32_t id, bool fence) {
    hardware::VirtioGpu& gpu = device_.gpu();
    CtxResource detach{header(kCmdCtxDetachResource), id, 0};
    ResourceUnref unref{header(kCmdResourceUnref), id, 0};
    gpu.command(as_bytes(detach), fence);
    gpu.command(as_bytes(unref), fence);
  }
  // The buffer moves to a new host resource (no fences: nothing here waits for the GPU). If the host will not make one, waiting for the GPU instead keeps the
  // old resource safe to write.
  void replace_busy_resource() {
    const uint32_t fresh = device_.gpu().allocate_resource_id();
    if (!make_resource(fresh, /*fence=*/false)) {
      drop_resource(fresh, /*fence=*/false);
      device_.wait_for_gpu();
      return;
    }
    drop_resource(id_, /*fence=*/false);
    id_ = fresh;
    __atomic_store_n(&last_use_, uint64_t{0}, __ATOMIC_RELEASE);   // (no work has the new one yet)
  }

  VirglDevice& device_;
  ByteView memory_;
  uint32_t id_ = 0;
  uint32_t bind_ = 0;
  uint64_t last_use_ = 0;    // the work number of the last submit that used the buffer (0: none)
  uint64_t accounted_ = 0;   // what the GPU's video memory figure counts for this buffer
  bool ok_ = false;
};

class VirglImage final : public IImage {
 public:
  VirglImage(VirglDevice& device, const ImageDesc& desc) : device_(device), desc_(desc) {
    hardware::VirtioGpu& gpu = device.gpu();
    id_ = gpu.allocate_resource_id();
    const bool depth = desc.format == Format::D32Float;
    switch (desc.format) {
      case Format::R32G32B32A32Float: color_format_ = virgl::kFormatR32G32B32A32Float; break;
      case Format::R16G16B16A16Float: color_format_ = virgl::kFormatR16G16B16A16Float; break;
      case Format::R16G16B16A16Unorm: color_format_ = virgl::kFormatR16G16B16A16Unorm; break;
      case Format::R32G32B32A32Uint: color_format_ = virgl::kFormatR32G32B32A32Uint; break;
      case Format::R32G32B32A32Sint: color_format_ = virgl::kFormatR32G32B32A32Sint; break;
      case Format::R16G16B16A16Uint: color_format_ = virgl::kFormatR16G16B16A16Uint; break;
      case Format::R16G16B16A16Sint: color_format_ = virgl::kFormatR16G16B16A16Sint; break;
      case Format::R8G8B8A8Uint: color_format_ = virgl::kFormatR8G8B8A8Uint; break;
      case Format::R16G16B16A16Snorm: color_format_ = virgl::kFormatR16G16B16A16Snorm; break;
      case Format::R10G10B10A2Unorm: color_format_ = virgl::kFormatR10G10B10A2Unorm; break;
      case Format::R32Uint: color_format_ = virgl::kFormatR32Uint; break;
      case Format::R32Sint: color_format_ = virgl::kFormatR32Sint; break;
      case Format::R32Float: color_format_ = virgl::kFormatR32Float; break;
      case Format::B8G8R8A8Srgb: case Format::R8G8B8A8Srgb: color_format_ = virgl::kFormatB8G8R8A8Srgb; break;
      case Format::Bc1Unorm: color_format_ = virgl::kFormatDxt1Rgba; break;
      case Format::Bc2Unorm: color_format_ = virgl::kFormatDxt3Rgba; break;
      case Format::Bc3Unorm: color_format_ = virgl::kFormatDxt5Rgba; break;
      case Format::Bc1Srgb: color_format_ = virgl::kFormatDxt1Srgba; break;
      case Format::Bc2Srgb: color_format_ = virgl::kFormatDxt3Srgba; break;
      case Format::Bc3Srgb: color_format_ = virgl::kFormatDxt5Srgba; break;
      // (R8G8B8A8Unorm too is a BGRA image on the host: pixels go in and come out as 0xAARRGGBB words whatever the format's memory order, and a shader sees colours, not bytes.
      // A storage image is an RGBA one - the host's image units have no BGRA format -, its pixels' red and blue swapped on the way in and out)
      default:
        color_format_ = (desc.usage & kImageStorage) ? virgl::kFormatR8G8B8A8Unorm : virgl::kFormatB8G8R8A8Unorm;
        swap_rb_ = (desc.usage & kImageStorage) != 0;
        break;
    }
    const uint32_t depth_format = desc.stencil ? virgl::kFormatZ32FloatS8X24Uint : virgl::kFormatZ32Float;
    // A sampled image is stored top row first, the way write_pixels() gives it and the sampler reads it, so it does not take the Y_0_TOP flip of the images that are only
    // drawn into (and shown).
    const bool sampled = (desc.usage & kImageSampled) != 0;
    uint32_t bind = depth ? virgl::kBindDepthStencil : is_compressed_format(desc.format) ? virgl::kBindSamplerView : (virgl::kBindRenderTarget | virgl::kBindSamplerView);
    if (desc.usage & kImagePresent) bind |= virgl::kBindScanout;
    // A volume has its slices as the depth; an array or a cube its layers as the array size (a cube: six).
    const uint32_t layers = layer_count();
    const uint32_t target = desc.kind == TextureKind::Texture3D ? virgl::kTargetTexture3d : desc.kind == TextureKind::TextureCube ? virgl::kTargetTextureCube
                            : desc.kind == TextureKind::Texture2DArray ? virgl::kTargetTexture2dArray : virgl::kTargetTexture2d;
    ResourceCreate3d create{header(kCmdResourceCreate3d), id_, target,
                            depth ? depth_format : color_format_, bind,
                            desc.extent.width, desc.extent.height, desc.kind == TextureKind::Texture3D ? layers : 1u, desc.kind == TextureKind::Texture3D || desc.kind == TextureKind::Texture2D ? 1u : layers, desc.mip_levels - 1 /* last level */, desc.samples > 1 ? desc.samples : 0u,
                            sampled || desc.kind != TextureKind::Texture2D ? 0u : kResourceFlagY0Top, 0};   // (the host takes the flag on 2D images alone: a layered depth buffer is kept the GL way)
    CtxResource ctx{header(kCmdCtxAttachResource), id_, 0};
    ok_ = gpu.command(as_bytes(create)) && gpu.command(as_bytes(ctx));
    accounted_ = uint64_t{desc.extent.width} * desc.extent.height * layer_count() * desc.samples * (depth ? 4u : bytes_per_pixel(desc.format));
    gpu.add_resource_bytes(accounted_);
    // The surface (a render-target view of the resource) is what the framebuffer state refers to.
    if (ok_ && (desc.usage & (kImageColorAttachment | kImageDepthAttachment))) {
      surface_ = surface(0, 1);
      ok_ = surface_ != 0;
    }
    // The sampler view is what a texture unit is bound to: the whole resource, all of its levels, components in order.
    if (ok_ && sampled) {
      view_ = device.new_handle();
      // (the layers the view shows: all of an array's or a cube's; a volume's slices are not layers)
      const uint32_t last_layer = desc.kind == TextureKind::Texture2DArray || desc.kind == TextureKind::TextureCube ? layers - 1 : 0;
      const uint32_t cmd[] = {virgl::header(virgl::kCreateObject, virgl::kObjectSamplerView, 6), view_, id_, color_format_, last_layer << 16, (desc.mip_levels - 1) << 8 /* first level | last level << 8 */, virgl::kSwizzleRgba};
      ok_ = device.send_commands(cmd, 7);
    }
  }
  ~VirglImage() {
    hardware::VirtioGpu& gpu = device_.gpu();
    if (view_) device_.destroy_object(view_);
    for (const LayerSurface& s : surfaces_)
      if (s.handle) device_.destroy_object(s.handle);
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
  // The host's surface of layers first .. first + count - 1 (a layered one draws into the layer gl_Layer names, from the first), made on first use; 0 if the host refuses it.
  uint32_t surface(uint32_t first, uint32_t count) {
    for (const LayerSurface& s : surfaces_)
      if (s.handle && s.first == first && s.count == count) return s.handle;
    LayerSurface* free = nullptr;
    for (LayerSurface& s : surfaces_)
      if (!s.handle) { free = &s; break; }
    if (!free || count == 0 || first + count > layer_count()) return 0;
    const uint32_t handle = device_.new_handle();
    const uint32_t fmt = desc_.format == Format::D32Float ? (desc_.stencil ? virgl::kFormatZ32FloatS8X24Uint : virgl::kFormatZ32Float) : color_format_;
    const uint32_t cmd[] = {virgl::header(virgl::kCreateObject, virgl::kObjectSurface, 5), handle, id_, fmt, 0 /* level */, first | ((first + count - 1) << 16)};
    if (!device_.send_commands(cmd, 6)) return 0;
    *free = LayerSurface{first, count, handle};
    return handle;
  }
  // The format a storage image is bound with (and what its shaders' IMGFMT<n> is): the image's own, or the view's of the same pixel size.
  uint32_t image_format(Format view) const { return view == Format::Undefined || view == desc_.format ? color_format_ : host_format(view); }
  static uint32_t host_format(Format f) {
    switch (f) {
      case Format::R32G32B32A32Float: return virgl::kFormatR32G32B32A32Float;
      case Format::R16G16B16A16Float: return virgl::kFormatR16G16B16A16Float;
      case Format::R16G16B16A16Unorm: return virgl::kFormatR16G16B16A16Unorm;
      case Format::R32G32B32A32Uint: return virgl::kFormatR32G32B32A32Uint;
      case Format::R32G32B32A32Sint: return virgl::kFormatR32G32B32A32Sint;
      case Format::R16G16B16A16Uint: return virgl::kFormatR16G16B16A16Uint;
      case Format::R16G16B16A16Sint: return virgl::kFormatR16G16B16A16Sint;
      case Format::R8G8B8A8Uint: return virgl::kFormatR8G8B8A8Uint;
      case Format::R16G16B16A16Snorm: return virgl::kFormatR16G16B16A16Snorm;
      case Format::R10G10B10A2Unorm: return virgl::kFormatR10G10B10A2Unorm;
      case Format::R32Uint: return virgl::kFormatR32Uint;
      case Format::R32Sint: return virgl::kFormatR32Sint;
      case Format::R32Float: return virgl::kFormatR32Float;
      default: return virgl::kFormatR8G8B8A8Unorm;
    }
  }
  uint32_t view() const { return view_; }
  Extent2D extent() const override { return desc_.extent; }
  Format format() const override { return desc_.format; }
  uint32_t usage() const { return desc_.usage; }
  uint32_t samples() const { return desc_.samples; }
  bool has_stencil() const { return desc_.stencil; }
  uint32_t blit_format() const { return desc_.format == Format::D32Float ? (desc_.stencil ? virgl::kFormatZ32FloatS8X24Uint : virgl::kFormatZ32Float) : color_format_; }   // (what a blit calls the image's format)
  uint32_t color_format() const { return color_format_; }
  bool stencil() const { return desc_.stencil; }
  uint32_t layer_count() const override { return desc_.kind == TextureKind::Texture2D ? 1 : desc_.kind == TextureKind::TextureCube ? 6 : desc_.layers; }
  uint32_t mip_levels() const override { return desc_.mip_levels; }
  // A level above 0 of a mipmapped texture: staged at the start of the backing (which has room for level 0, the largest), from where the host copies it into the level.
  bool write_mip_pixels(uint32_t level, ArrayView<const uint32_t> pixels) override {
    if (level == 0) return write_pixels(pixels);
    const uint32_t width = mip_extent(desc_.extent.width, level), height = mip_extent(desc_.extent.height, level);
    const uint64_t count = uint64_t{width} * height;
    if (level >= desc_.mip_levels || pixels.size() != count || !ensure_backing(uint64_t{desc_.extent.width} * desc_.extent.height)) return false;
    const ArrayView<uint32_t> staged = backing_.cast<uint32_t>();
    for (uint64_t i = 0; i < count; ++i) staged[i] = pixels[i];
    swap_words(staged.first(count));
    TransferHost3d transfer{header(kCmdTransferToHost3d), {0, 0, 0, width, height, 1}, 0, id_, level, width * 4, 0};
    return device_.gpu().command(as_bytes(transfer), /*fence=*/false);
  }
  // The blocks of a level of a layer (a slice, a face; 0 for a plain texture) of a block-compressed image: staged at the start of the backing (room for level 0 of a layer, as
  // 32-bit pixels, which is more than its blocks), the transfer's stride a row of blocks, its box in pixels.
  bool write_compressed(uint32_t level, uint32_t layer, ConstByteView blocks) override {
    if (!is_compressed_format(desc_.format) || level >= desc_.mip_levels || layer >= layer_count()) return false;
    const uint32_t width = mip_extent(desc_.extent.width, level), height = mip_extent(desc_.extent.height, level);
    if (blocks.size() != compressed_image_bytes(desc_.format, width, height) || !ensure_backing(uint64_t{desc_.extent.width} * desc_.extent.height)) return false;
    ByteView staged;
    if (!backing_.try_subview(0, blocks.size(), &staged) || !staged.copy_from(blocks)) return false;
    TransferHost3d transfer{header(kCmdTransferToHost3d), {0, 0, layer, width, height, 1}, 0, id_, level, static_cast<uint32_t>(compressed_row_bytes(desc_.format, width)), 0};
    return device_.gpu().command(as_bytes(transfer), /*fence=*/false);
  }
  // A layer (a slice, a face) goes through the one-layer backing: the host copies from or into it at the layer's place in the resource.
  bool write_layer_pixels(uint32_t layer, ArrayView<const uint32_t> pixels) override {
    const uint64_t count = uint64_t{desc_.extent.width} * desc_.extent.height;
    if (desc_.kind == TextureKind::Texture2D || layer >= layer_count() || pixels.size() != count || !ensure_backing(count)) return false;
    const ArrayView<uint32_t> staged = backing_.cast<uint32_t>();
    for (uint64_t i = 0; i < count; ++i) staged[i] = pixels[i];
    swap_words(staged.first(count));
    TransferHost3d transfer{header(kCmdTransferToHost3d), {0, 0, layer, desc_.extent.width, desc_.extent.height, 1}, 0, id_, 0, desc_.extent.width * 4, 0};
    return device_.gpu().command(as_bytes(transfer), /*fence=*/false);
  }
  bool write_layer_pixels_float(uint32_t layer, ArrayView<const Vec4> pixels) override {
    const uint64_t count = uint64_t{desc_.extent.width} * desc_.extent.height;
    if (!is_wide_format(desc_.format) || desc_.kind == TextureKind::Texture2D || layer >= layer_count() || pixels.size() != count || !ensure_backing(count)) return false;
    ByteWriter out{backing_};
    for (const Vec4& c : pixels) encode_wide_pixel(desc_.format, c, out);
    if (!out.ok()) return false;
    TransferHost3d transfer{header(kCmdTransferToHost3d), {0, 0, layer, desc_.extent.width, desc_.extent.height, 1}, 0, id_, 0, desc_.extent.width * bytes_per_pixel(desc_.format), 0};
    return device_.gpu().command(as_bytes(transfer), /*fence=*/false);
  }
  bool read_layer_pixels(uint32_t layer, ArrayView<uint32_t> out) override {
    const uint64_t count = uint64_t{desc_.extent.width} * desc_.extent.height;
    if (desc_.kind == TextureKind::Texture2D || layer >= layer_count() || out.size() < count || !ensure_backing(count)) return false;
    TransferHost3d transfer{header(kCmdTransferFromHost3d), {0, 0, layer, desc_.extent.width, desc_.extent.height, 1}, 0, id_, 0, desc_.extent.width * 4, 0};
    if (!device_.gpu().command(as_bytes(transfer))) return false;
    const ArrayView<uint32_t> pixels = backing_.cast<uint32_t>();
    for (uint64_t i = 0; i < count; ++i) out[i] = pixels[i];
    swap_words(out.first(count));
    return true;
  }
  bool read_layer_pixels_float(uint32_t layer, ArrayView<Vec4> out) override {
    const uint64_t count = uint64_t{desc_.extent.width} * desc_.extent.height;
    if (!is_wide_format(desc_.format) || desc_.kind == TextureKind::Texture2D || layer >= layer_count() || out.size() < count || !ensure_backing(count)) return false;
    TransferHost3d transfer{header(kCmdTransferFromHost3d), {0, 0, layer, desc_.extent.width, desc_.extent.height, 1}, 0, id_, 0, desc_.extent.width * bytes_per_pixel(desc_.format), 0};
    if (!device_.gpu().command(as_bytes(transfer))) return false;
    ByteReader in{ConstByteView(backing_)};
    for (uint64_t i = 0; i < count; ++i) out[i] = decode_wide_pixel(desc_.format, in);
    return in.ok();
  }
  // A level above 0 (what a storage image's shaders wrote there): staged at the start of the backing.
  bool read_mip_pixels(uint32_t level, ArrayView<uint32_t> out) override {
    if (level == 0) return read_pixels(out);
    const uint32_t width = mip_extent(desc_.extent.width, level), height = mip_extent(desc_.extent.height, level);
    const uint64_t count = uint64_t{width} * height;
    if (level >= desc_.mip_levels || is_wide_format(desc_.format) || out.size() < count || !ensure_backing(uint64_t{desc_.extent.width} * desc_.extent.height)) return false;
    TransferHost3d transfer{header(kCmdTransferFromHost3d), {0, 0, 0, width, height, 1}, 0, id_, level, width * 4, 0};
    if (!device_.gpu().command(as_bytes(transfer))) return false;
    const ArrayView<uint32_t> pixels = backing_.cast<uint32_t>();
    for (uint64_t i = 0; i < count; ++i) out[i] = pixels[i];
    swap_words(out.first(count));
    return true;
  }
  // Stored with window y 0 at the top (drawn into and shown: the Y_0_TOP flag), or the way a texture is - row 0 first, window y 0 at the bottom of GL's window - which is what a
  // render target that is also sampled needs, so that the sampler reads it the way a texture is read (drawn upright by an unflipped viewport, see VirglPipeline).
  bool y0_top() const { return !(desc_.usage & kImageSampled) && desc_.kind == TextureKind::Texture2D; }

  bool read_pixels(ArrayView<uint32_t> out) override {
    const uint64_t count = uint64_t{desc_.extent.width} * desc_.extent.height;
    if (desc_.format == Format::D32Float || is_wide_format(desc_.format) || out.size() < count) return false;
    hardware::VirtioGpu& gpu = device_.gpu();
    if (!ensure_backing(count)) return false;
    TransferHost3d transfer{header(kCmdTransferFromHost3d),
                            {0, 0, 0, desc_.extent.width, desc_.extent.height, 1}, 0, id_, 0,
                            desc_.extent.width * 4, 0};
    if (!gpu.command(as_bytes(transfer))) return false;
    const ArrayView<uint32_t> pixels = backing_.cast<uint32_t>();
    for (uint64_t i = 0; i < count; ++i) out[i] = pixels[i];
    swap_words(out.first(count));
    return true;
  }

  bool read_pixels_float(ArrayView<Vec4> out) override {
    const uint64_t count = uint64_t{desc_.extent.width} * desc_.extent.height;
    if (!is_wide_format(desc_.format) || out.size() < count || !ensure_backing(count)) return false;
    TransferHost3d transfer{header(kCmdTransferFromHost3d), {0, 0, 0, desc_.extent.width, desc_.extent.height, 1}, 0, id_, 0, desc_.extent.width * bytes_per_pixel(desc_.format), 0};
    if (!device_.gpu().command(as_bytes(transfer))) return false;
    ByteReader in{ConstByteView(backing_)};
    for (uint64_t i = 0; i < count; ++i) out[i] = decode_wide_pixel(desc_.format, in);
    return in.ok();
  }

  bool write_pixels_float(ArrayView<const Vec4> pixels) override {
    const uint64_t count = uint64_t{desc_.extent.width} * desc_.extent.height;
    if (!is_wide_format(desc_.format) || desc_.samples > 1 || pixels.size() != count || !ensure_backing(count)) return false;
    ByteWriter out{backing_};
    for (const Vec4& c : pixels) encode_wide_pixel(desc_.format, c, out);
    if (!out.ok()) return false;
    TransferHost3d transfer{header(kCmdTransferToHost3d), {0, 0, 0, desc_.extent.width, desc_.extent.height, 1}, 0, id_, 0, desc_.extent.width * bytes_per_pixel(desc_.format), 0};
    return device_.gpu().command(as_bytes(transfer), /*fence=*/false);
  }

  // The host copies the pixels from guest memory (the backing) into the resource: a texture, or an image that is only drawn into and shown (its Y_0_TOP rows go
  // top row first as read_pixels() gives them back, so a swapchain image the CPU cleared or copied into is shown as written). Not a depth or multisampled image.
  bool write_pixels(ArrayView<const uint32_t> pixels) override {
    const uint64_t count = uint64_t{desc_.extent.width} * desc_.extent.height;
    if (desc_.samples > 1 || desc_.format == Format::D32Float || is_wide_format(desc_.format) || pixels.size() != count) return false;
    if (!ensure_backing(count)) return false;
    const ArrayView<uint32_t> staged = backing_.cast<uint32_t>();
    for (uint64_t i = 0; i < count; ++i) staged[i] = pixels[i];
    swap_words(staged.first(count));
    TransferHost3d transfer{header(kCmdTransferToHost3d), {0, 0, 0, desc_.extent.width, desc_.extent.height, 1}, 0, id_, 0, desc_.extent.width * 4, 0};
    return device_.gpu().command(as_bytes(transfer), /*fence=*/false);
  }

  // A rectangle: its rows go to the same place in the backing (the backing is the whole image), and the host copies just that box.
  bool write_rect(const Rect2D& area, ArrayView<const uint32_t> pixels, uint32_t pitch) override {
    const Extent2D e = desc_.extent;
    if (desc_.samples > 1 || desc_.format == Format::D32Float || is_wide_format(desc_.format) || !area.width || !area.height || pitch < area.width) return false;
    if (area.x < 0 || area.y < 0 || uint64_t{static_cast<uint32_t>(area.x)} + area.width > e.width || uint64_t{static_cast<uint32_t>(area.y)} + area.height > e.height) return false;
    if (pixels.size() < uint64_t{area.height - 1} * pitch + area.width) return false;
    if (!ensure_backing(uint64_t{e.width} * e.height)) return false;
    const ArrayView<uint32_t> staged = backing_.cast<uint32_t>();
    for (uint32_t j = 0; j < area.height; ++j)
      for (uint32_t i = 0; i < area.width; ++i) staged[uint64_t{static_cast<uint32_t>(area.y) + j} * e.width + static_cast<uint32_t>(area.x) + i] = swap_word(pixels[uint64_t{j} * pitch + i]);
    const uint64_t offset = (uint64_t{static_cast<uint32_t>(area.y)} * e.width + static_cast<uint32_t>(area.x)) * 4;
    TransferHost3d transfer{header(kCmdTransferToHost3d), {static_cast<uint32_t>(area.x), static_cast<uint32_t>(area.y), 0, area.width, area.height, 1}, offset, id_, 0, e.width * 4, 0};
    return device_.gpu().command(as_bytes(transfer), /*fence=*/false);
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

  // (a storage image of 8-bit colour is RGBA on the host: the 0xAARRGGBB words it is written and read in have red and blue turned around)
  uint32_t swap_word(uint32_t w) const { return swap_rb_ ? (w & 0xFF00FF00u) | ((w & 0xFF) << 16) | ((w >> 16) & 0xFF) : w; }
  void swap_words(ArrayView<uint32_t> words) const {
    if (!swap_rb_) return;
    for (uint32_t& w : words) w = swap_word(w);
  }
  struct LayerSurface {
    uint32_t first = 0, count = 0, handle = 0;
  };
  static constexpr uint32_t kLayerSurfaces = 8;   // (the layer ranges render target views name: a few per image)
  LayerSurface surfaces_[kLayerSurfaces];
  bool swap_rb_ = false;
  VirglDevice& device_;
  ImageDesc desc_;
  uint32_t id_ = 0, surface_ = 0, view_ = 0, color_format_ = 0;
  uint64_t accounted_ = 0;   // what the GPU's video memory figure counts for this image
  ByteView backing_;
  bool ok_ = false;
};

class VirglShaderModule final : public IShaderModule {
 public:
  // `stream_output`: the declaration of a pipeline whose geometry shader this is (the host takes it with the shader).
  // `swap_stream`: the host rasterizes stream 0 only, so a pipeline that draws another stream has the numbers of that stream and stream 0 exchanged (the declaration here; the shader text is the caller's).
  VirglShaderModule(VirglDevice& device, const ShaderModuleDesc& desc, const GraphicsPipelineDesc* stream_output = nullptr, uint32_t swap_stream = 0)
      : stage_(desc.stage), tgsi_(desc.tgsi), tess_output_(desc.tess_output), tess_partitioning_(desc.tess_partitioning),
        capture_vertices_(desc.geometry_streams > 1 || desc.geometry_output == GeometryTopology::PointList ? 1u : desc.geometry_output == GeometryTopology::LineStrip ? 2u : 3u) {
    for (uint32_t k = 0; k < 3; ++k) thread_group_[k] = desc.thread_group[k];
    streams_ = desc.geometry_streams;
    output_topology_ = desc.geometry_output;
    desc_ = desc;
    // A shader with storage images names their formats IMGFMT<n> (ShaderModuleDesc::tgsi): the host's shader is made for the formats of the images a dispatch or draw binds
    // (variant), not now.
    if (find(desc.tgsi, "IMGFMT") >= 0 && (desc.stage == ShaderStage::Compute || desc.stage == ShaderStage::Fragment)) {
      has_images_ = true;
      ok_ = true;
      return;
    }
    handle_ = device.new_handle();
    const uint32_t length = string_length(desc.tgsi) + 1;  // with the terminating NUL
    // A long text goes in pieces (the host's continuation packets: the first says the whole length, each one after it its offset with bit 31 set), each a command of its own with
    // the same header: a virgl command counts its dwords in 16 bits, and the request buffer of the virtio queue is not unbounded either.
    constexpr uint32_t kPieceBytes = 32 * 1024;
    const uint32_t piece_dwords = ((length < kPieceBytes ? length : kPieceBytes) + 3) / 4;
    const uint32_t outputs = stream_output ? stream_output->stream_output_count : 0;
    const uint32_t so_dwords = outputs ? 4 + 2 * outputs : 0;   // (the four strides, and two words for each output)
    const uint32_t total = 6 + so_dwords + piece_dwords;
    hardware::VirtioGpu& gpu = device.gpu();
    uint32_t* cmd = static_cast<uint32_t*>(gpu.allocator().allocate(total * 4));
    if (!cmd) return;
    cmd[1] = handle_;
    cmd[2] = desc.stage == ShaderStage::Vertex ? virgl::kShaderVertex : desc.stage == ShaderStage::Compute ? virgl::kShaderCompute : desc.stage == ShaderStage::Geometry ? virgl::kShaderGeometry : desc.stage == ShaderStage::Hull ? virgl::kShaderTessControl : desc.stage == ShaderStage::Domain ? virgl::kShaderTessEval : virgl::kShaderFragment;
    cmd[4] = length < 300 ? 300 : length;    // upper bound on the TGSI token count the host reserves
    cmd[5] = desc.stage == ShaderStage::Compute ? desc.shared_memory_bytes : outputs;   // stream output outputs; for a compute shader the group-shared memory it needs, in bytes
    if (outputs) {
      for (uint32_t b = 0; b < 4; ++b) cmd[6 + b] = stream_output->stream_output_stride[b] / 4;   // (strides in words)
      uint32_t fill[4] = {};   // (the words of each buffer's record the entries so far take)
      for (uint32_t e = 0; e < outputs; ++e) {
        const StreamOutputEntry& entry = stream_output->stream_output[e];
        // (the shader keeps the position as the shader wrote it, +Y up, in an output of its own for each stream as well as in the host's output 0)
        const uint32_t reg = entry.output == 0 ? device.limits().varyings + 1 + entry.stream : entry.output;   // (the register after the varyings: D3d9Tgsi.cpp extra())
        cmd[10 + 2 * e] = reg | (entry.start_component << 8) | (entry.component_count << 10) | (entry.buffer << 13) | (fill[entry.buffer] << 16);
        cmd[11 + 2 * e] = entry.stream == swap_stream ? 0u : entry.stream == 0 ? swap_stream : entry.stream;
        fill[entry.buffer] += entry.component_count;
      }
    }
    ok_ = true;
    for (uint32_t at = 0; at < length && ok_; at += kPieceBytes) {
      const uint32_t bytes = length - at < kPieceBytes ? length - at : kPieceBytes;   // (with the NUL in the last piece)
      const uint32_t dwords = (bytes + 3) / 4;
      cmd[0] = virgl::header(virgl::kCreateObject, virgl::kObjectShader, 5 + so_dwords + dwords);
      cmd[3] = at == 0 ? length : (at | 0x80000000u);   // (VIRGL_OBJ_SHADER_OFFSET_CONT)
      cmd[6 + so_dwords + dwords - 1] = 0;
      __builtin_memcpy(&cmd[6 + so_dwords], desc.tgsi + at, at + bytes == length ? bytes - 1 : bytes);
      ok_ = device.send_commands(cmd, 6 + so_dwords + dwords);
    }
    gpu.allocator().deallocate(cmd);
  }
  bool valid() const { return ok_; }
  ShaderStage stage() const override { return stage_; }
  uint32_t handle() const { return handle_; }
  const char* tgsi() const { return tgsi_; }   // (the text the kernel keeps for the module's life)
  bool has_images() const { return has_images_; }
  // The host's shader for storage images of the formats `formats` (VIRGL_FORMAT_*, by slot): IMGFMT<n> made the name of slot n's format; made on first use and kept with the
  // module (kVariants of them; the oldest goes when there is no room). 0 when the host refuses it.
  uint32_t variant(VirglDevice& device, const uint32_t (&formats)[kMaxStorageImages]) const {
    for (const Variant& v : variants_)
      if (v.handle && __builtin_memcmp(v.formats, formats, sizeof formats) == 0) return v.handle;
    const uint32_t length = string_length(tgsi_);
    // (each IMGFMT<n> - seven characters - becomes a format's name, at most 32)
    uint32_t marks = 0;
    for (const char* p = tgsi_; (p = next_mark(p)) != nullptr; p += 6) ++marks;
    const uint64_t capacity = uint64_t{length} + uint64_t{marks} * 32 + 1;
    char* text = static_cast<char*>(device.gpu().allocator().allocate(capacity));
    if (!text) return 0;
    uint64_t n = 0;
    for (const char* p = tgsi_; *p;) {
      if (p[0] == 'I' && next_mark(p) == p && p[6] >= '0' && p[6] < '0' + static_cast<char>(kMaxStorageImages)) {
        const char* name = format_name(formats[p[6] - '0']);
        for (const char* c = name; *c && n + 1 < capacity; ++c) text[n++] = *c;
        p += 7;
        continue;
      }
      if (n + 1 < capacity) text[n++] = *p;
      ++p;
    }
    text[n] = 0;
    ShaderModuleDesc made = desc_;
    made.tgsi = text;
    VirglShaderModule module(device, made);
    device.gpu().allocator().deallocate(text);
    if (!module.valid() || !module.handle()) return 0;
    Variant& slot = variants_[next_variant_];
    next_variant_ = (next_variant_ + 1) % kVariants;
    if (slot.handle) device.destroy_object(slot.handle);
    slot.handle = module.handle();
    for (uint32_t k = 0; k < kMaxStorageImages; ++k) slot.formats[k] = formats[k];
    return slot.handle;
  }
  void destroy_variants(VirglDevice& device) const {
    for (Variant& v : variants_)
      if (v.handle) device.destroy_object(v.handle), v.handle = 0;
  }
  uint32_t streams() const { return streams_; }
  GeometryTopology output_topology() const { return output_topology_; }
  uint32_t capture_vertices() const { return capture_vertices_; }   // (a geometry shader: the vertices of a primitive it captures)
  TessOutput tess_output() const { return tess_output_; }
  TessPartitioning tess_partitioning() const { return tess_partitioning_; }
  uint32_t thread_group(uint32_t axis) const { return thread_group_[axis]; }

 private:
  // The place of `what` in `text` (-1: none).
  static int64_t find(const char* text, const char* what) {
    for (int64_t i = 0; text[i]; ++i) {
      int64_t k = 0;
      while (what[k] && text[i + k] == what[k]) ++k;
      if (!what[k]) return i;
    }
    return -1;
  }
  static const char* next_mark(const char* p) {
    const int64_t at = find(p, "IMGFMT");
    return at < 0 ? nullptr : p + at;
  }
  // The TGSI name of a storage image's format (Gallium's util_format names, which the host's parser takes).
  static const char* format_name(uint32_t format) {
    switch (format) {
      case virgl::kFormatR32G32B32A32Float: return "PIPE_FORMAT_R32G32B32A32_FLOAT";
      case virgl::kFormatR16G16B16A16Float: return "PIPE_FORMAT_R16G16B16A16_FLOAT";
      case virgl::kFormatR16G16B16A16Unorm: return "PIPE_FORMAT_R16G16B16A16_UNORM";
      case virgl::kFormatR16G16B16A16Snorm: return "PIPE_FORMAT_R16G16B16A16_SNORM";
      case virgl::kFormatR32G32B32A32Uint: return "PIPE_FORMAT_R32G32B32A32_UINT";
      case virgl::kFormatR32G32B32A32Sint: return "PIPE_FORMAT_R32G32B32A32_SINT";
      case virgl::kFormatR16G16B16A16Uint: return "PIPE_FORMAT_R16G16B16A16_UINT";
      case virgl::kFormatR16G16B16A16Sint: return "PIPE_FORMAT_R16G16B16A16_SINT";
      case virgl::kFormatR8G8B8A8Uint: return "PIPE_FORMAT_R8G8B8A8_UINT";
      case virgl::kFormatR10G10B10A2Unorm: return "PIPE_FORMAT_R10G10B10A2_UNORM";
      case virgl::kFormatR32Uint: return "PIPE_FORMAT_R32_UINT";
      case virgl::kFormatR32Sint: return "PIPE_FORMAT_R32_SINT";
      case virgl::kFormatR32Float: return "PIPE_FORMAT_R32_FLOAT";
      default: return "PIPE_FORMAT_R8G8B8A8_UNORM";
    }
  }
  struct Variant {
    uint32_t formats[kMaxStorageImages] = {};
    uint32_t handle = 0;
  };
  static constexpr uint32_t kVariants = 4;
  mutable Variant variants_[kVariants];
  mutable uint32_t next_variant_ = 0;
  bool has_images_ = false;
  ShaderModuleDesc desc_;
  uint32_t thread_group_[3] = {1, 1, 1};
  ShaderStage stage_;
  const char* tgsi_ = nullptr;
  TessOutput tess_output_ = TessOutput::TriangleCcw;
  TessPartitioning tess_partitioning_ = TessPartitioning::Integer;
  uint32_t capture_vertices_ = 3;
  uint32_t streams_ = 1;
  GeometryTopology output_topology_ = GeometryTopology::TriangleStrip;
  uint32_t handle_ = 0;
  bool ok_ = false;
};

// Both kinds of pipeline, so that destroy_pipeline can tell them apart.
// The host's vertex element format of a VertexFormat: VIRGL_FORMAT_* runs R, RG, RGB, RGBA for each component type.
uint32_t virgl_vertex_format(VertexFormat format) {
  static const uint32_t kFirst[] = {virgl::kFormatR32Float, 64 /* R8_UNORM */, 74 /* R8_SNORM */, 177 /* R8_UINT */, 181 /* R8_SINT */, 48 /* R16_UNORM */, 56 /* R16_SNORM */,
                                    185 /* R16_UINT */, 189 /* R16_SINT */, 91 /* R16_FLOAT */, 193 /* R32_UINT */, 197 /* R32_SINT */};
  if (format == VertexFormat::Bgra8Unorm) return virgl::kFormatB8G8R8A8Unorm;
  if (format == VertexFormat::Rgb10A2Unorm) return virgl::kFormatR10G10B10A2Unorm;
  return kFirst[static_cast<uint32_t>(vertex_format_type(format))] + vertex_format_components(format) - 1;
}

// --- Depth bias of a normalised depth format --------------------------------------------------------------------------------------------------------------------------------------
// The host's depth buffers are 32-bit floats (Z32F_S8X24), and its polygon offset counts in a float buffer's unit, 2^(e - 23) for the largest depth e of the triangle - which is
// Direct3D's unit for a float format. A 16- or 24-bit format's unit is a fixed step, 1 / (2^n - 1): the constant part of such a bias cannot be had from the polygon offset, so the
// pipeline's fragment shader gets the whole bias added to the fragment's depth. The text of that shader: the bias is worked out at the start (the slope from the derivatives of the
// window depth, while every fragment of the quad still runs), and the depth is written wherever the shader ends.
// (a C string's bytes, and bytes ending in a NUL as the C string they hold: char and uint8_t have the same size and alignment, and either view lives as long as what it views)
ConstByteView text_view(const char* text) { return ConstByteView(reinterpret_cast<const uint8_t*>(text), string_length(text)); }
const char* text_chars(ConstByteView bytes) { return reinterpret_cast<const char*>(bytes.data()); }
bool text_starts(ConstByteView text, uint64_t at, const char* what) {
  for (uint64_t k = 0; what[k]; ++k)
    if (at + k >= text.size() || text[at + k] != static_cast<uint8_t>(what[k])) return false;
  return true;
}
int64_t text_find(ConstByteView text, const char* what, uint64_t from = 0) {
  for (uint64_t at = from; at < text.size(); ++at)
    if (text_starts(text, at, what)) return static_cast<int64_t>(at);
  return -1;
}
bool put_text(ByteBuffer& out, const char* what) { return out.append(text_view(what)); }
bool put_number(ByteBuffer& out, uint32_t v) {
  char digits[11];
  uint32_t n = 0;
  do {
    digits[n++] = static_cast<char>('0' + v % 10);
    v /= 10;
  } while (v && n < sizeof digits);
  bool ok = true;
  while (n) ok = ok && out.put8(static_cast<uint8_t>(digits[--n]));
  return ok;
}
bool depth_bias_fragment_text(ConstByteView text, const DepthBias& bias, ByteBuffer& out) {
  // (the shader's colour outputs come first: the depth is the next output; the temporary after the shader's own; the immediates are numbered in order)
  uint32_t outputs = 0, immediates = 0, temps = 0;
  for (int64_t at = text_find(text, "DCL OUT["); at >= 0; at = text_find(text, "DCL OUT[", static_cast<uint64_t>(at) + 1)) ++outputs;
  for (uint64_t at = 0; at < text.size(); ++at)
    if ((at == 0 || text[at - 1] == '\n') && text_starts(text, at, "IMM[")) ++immediates;
  const int64_t temp_at = text_find(text, "DCL TEMP[0..");
  if (temp_at < 0 || outputs == 0) return false;
  for (uint64_t at = static_cast<uint64_t>(temp_at) + 12; at < text.size() && text[at] >= '0' && text[at] <= '9' && temps < 100000; ++at) temps = temps * 10 + (text[at] - '0');
  const uint32_t temp = temps + 1;
  const bool has_position = text_find(text, "DCL IN[16], POSITION") >= 0;
  const int64_t const_at = text_find(text, "DCL CONST[");
  // (the first instruction: a line of the body, "  <n>: ...")
  int64_t first = -1;
  for (uint64_t at = 0; at < text.size() && first < 0; ++at) {
    if (at != 0 && text[at - 1] != '\n') continue;
    uint64_t k = at;
    while (k < text.size() && text[k] == ' ') ++k;
    const uint64_t digits = k;
    while (k < text.size() && text[k] >= '0' && text[k] <= '9') ++k;
    if (k > digits && k < text.size() && text[k] == ':') first = static_cast<int64_t>(at);
  }
  if (const_at < 0 || first < 0 || const_at > first) return false;
  auto reg = [&](const char* before, uint32_t number, const char* after) { return put_text(out, before) && put_number(out, number) && put_text(out, after); };
  bool ok = out.append(text.first(static_cast<uint64_t>(const_at)));
  if (!has_position) ok = ok && put_text(out, "DCL IN[16], POSITION, LINEAR\n");
  ok = ok && reg("DCL OUT[", outputs, "], POSITION\n") && reg("DCL TEMP[", temp, "]\n");
  ok = ok && out.append(text.subview(static_cast<uint64_t>(const_at), static_cast<uint64_t>(first - const_at)));
  const float constant = static_cast<float>(bias.constant) * depth_bias_unit(bias, 0.0f);
  ok = ok && reg("IMM[", immediates, "] UINT32 {") && put_number(out, float_bits(constant)) && put_text(out, ", ") && put_number(out, float_bits(bias.slope)) && put_text(out, ", ") &&
       put_number(out, float_bits(bias.clamp)) && put_text(out, ", 0}\n");
  // bias = clamp(constant + slope * max(|ddx z|, |ddy z|)) in TEMP[t].x
  ok = ok && reg("  DDX TEMP[", temp, "].x, IN[16].zzzz\n") && reg("  DDY TEMP[", temp, "].y, IN[16].zzzz\n");
  ok = ok && reg("  MAX TEMP[", temp, "].x, |TEMP[") && reg("", temp, "].xxxx|, |TEMP[") && reg("", temp, "].yyyy|\n");
  ok = ok && reg("  MAD TEMP[", temp, "].x, TEMP[") && reg("", temp, "].xxxx, IMM[") && reg("", immediates, "].yyyy, IMM[") && reg("", immediates, "].xxxx\n");
  if (bias.clamp != 0.0f) ok = ok && reg(bias.clamp > 0.0f ? "  MIN TEMP[" : "  MAX TEMP[", temp, "].x, TEMP[") && reg("", temp, "].xxxx, IMM[") && reg("", immediates, "].zzzz\n");
  // (the depth, wherever the shader ends: before every RET and the END)
  auto write_depth = [&]() {
    return reg("  ADD TEMP[", temp, "].y, IN[16].zzzz, TEMP[") && reg("", temp, "].xxxx\n") && reg("  MOV_SAT OUT[", outputs, "].z, TEMP[") && reg("", temp, "].yyyy\n");
  };
  uint64_t at = static_cast<uint64_t>(first);
  while (ok && at < text.size()) {
    uint64_t end = at;
    while (end < text.size() && text[end] != '\n') ++end;
    if (end < text.size()) ++end;
    const ConstByteView line_text = text.subview(at, end - at);
    if (text_find(line_text, ": RET") >= 0 || text_find(line_text, ": END") >= 0) ok = ok && write_depth();
    ok = ok && out.append(line_text);
    at = end;
  }
  return ok && out.put8(0);
}

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
  explicit VirglComputePipeline(const VirglShaderModule& shader) : VirglPipelineBase(true), shader_(shader.handle()), module_(&shader) {
    for (uint32_t k = 0; k < 3; ++k) group_[k] = shader.thread_group(k);
  }
  uint32_t shader() const { return shader_; }
  const VirglShaderModule* module() const { return module_; }   // (whose storage images' formats pick the host's shader: VirglShaderModule::variant)
  uint32_t group(uint32_t axis) const { return group_[axis]; }

 private:
  uint32_t shader_;
  const VirglShaderModule* module_;
  uint32_t group_[3];
};

class VirglPipeline final : public VirglPipelineBase {
 public:
  VirglPipeline(VirglDevice& device, const GraphicsPipelineDesc& desc)
      : VirglPipelineBase(false), device_(device), desc_(desc), vs_(static_cast<const VirglShaderModule*>(desc.vertex_shader)->handle()),
        fs_(static_cast<const VirglShaderModule*>(desc.fragment_shader)->handle()),
        gs_(desc.geometry_shader ? static_cast<const VirglShaderModule*>(desc.geometry_shader)->handle() : 0),
        hs_(desc.hull_shader ? static_cast<const VirglShaderModule*>(desc.hull_shader)->handle() : 0),
        ds_(desc.domain_shader ? static_cast<const VirglShaderModule*>(desc.domain_shader)->handle() : 0) {
    for (uint32_t e = 0; e < desc.stream_output_count; ++e) {
      so_buffers_ |= 1u << desc.stream_output[e].buffer;
      so_stream_of_buffer_[desc.stream_output[e].buffer] = desc.stream_output[e].stream;
    }
    if (desc.geometry_shader) capture_vertices_ = static_cast<const VirglShaderModule*>(desc.geometry_shader)->capture_vertices();
    else if (desc.hull_shader) capture_vertices_ = static_cast<const VirglShaderModule*>(desc.hull_shader)->tess_output() == TessOutput::Point ? 1 : static_cast<const VirglShaderModule*>(desc.hull_shader)->tess_output() == TessOutput::Line ? 2 : 3;
    // Stream output goes with the last vertex-processing shader on the host (the geometry shader, else the domain shader, else the vertex shader): a copy of it that has the declaration.
    if (desc.stream_output_count) {
      if (!gs_ && !ds_) {
        const auto* vertex = static_cast<const VirglShaderModule*>(desc.vertex_shader);
        if (!vertex->tgsi()) return;
        ShaderModuleDesc variant;
        variant.stage = ShaderStage::Vertex;
        variant.tgsi = vertex->tgsi();
        VirglShaderModule module(device, variant, &desc);
        if (!module.valid()) return;
        vs_ = module.handle();
      }
    }
    if (desc.stream_output_count && gs_) {
      const auto* geometry = static_cast<const VirglShaderModule*>(desc.geometry_shader);
      if (!geometry->tgsi()) return;
      // The host gives a register one stream: a shader output that two streams capture cannot be had.
      uint8_t streams_of_output[kMaxVaryings + 1] = {};
      for (uint32_t e = 0; e < desc.stream_output_count; ++e) {
        const StreamOutputEntry& entry = desc.stream_output[e];
        if (entry.output == 0) continue;
        streams_of_output[entry.output] |= 1u << entry.stream;
        if (streams_of_output[entry.output] & (streams_of_output[entry.output] - 1)) return;
      }
      // It rasterizes stream 0 only: when another is drawn, the numbers of that stream and 0 are exchanged in the text (the immediates the emits take, found by their marker) and in the declaration.
      const uint32_t drawn = desc.rasterized_stream > 0 ? static_cast<uint32_t>(desc.rasterized_stream) : 0;
      const uint32_t length = string_length(geometry->tgsi());
      char* text = static_cast<char*>(device.gpu().allocator().allocate(length + 1));
      if (!text) return;
      __builtin_memcpy(text, geometry->tgsi(), length + 1);
      if (drawn) {
        // ("{0, 0, 0, 24064}" is stream 0 and "{n, n, n, 2406(4 + n)}" stream n: the three digits are the stream)
        auto set_stream = [&](char stream_digit, char new_digit) {
          const char pattern[] = "{N, N, N, 2406W}";
          for (uint32_t i = 0; i + sizeof pattern - 1 <= length; ++i) {
            uint32_t k = 0;
            for (; k < sizeof pattern - 1; ++k) {
              const char want = pattern[k] == 'N' ? stream_digit : pattern[k] == 'W' ? static_cast<char>('4' + (stream_digit - '0')) : pattern[k];
              if (text[i + k] != want) break;
            }
            if (k == sizeof pattern - 1) text[i + 1] = text[i + 4] = text[i + 7] = new_digit;
          }
        };
        const char drawn_digit = static_cast<char>('0' + drawn);
        set_stream('0', drawn_digit);
        set_stream(drawn_digit, '0');
      }
      ShaderModuleDesc variant;
      variant.stage = ShaderStage::Geometry;
      variant.tgsi = text;
      variant.geometry_streams = geometry->streams();
      variant.geometry_output = geometry->output_topology();
      VirglShaderModule module(device, variant, &desc, drawn);
      device.gpu().allocator().deallocate(text);
      if (!module.valid()) return;
      gs_ = module.handle();
    }
    // The output primitive of the tessellator is the hull shader's to say (Direct3D) but the evaluation shader's TGSI (OpenGL): a domain shader made for another one gets a copy of its text
    // with the winding and point mode of the hull shader it is used with.
    if (hs_ && ds_) {
      const auto* hull = static_cast<const VirglShaderModule*>(desc.hull_shader);
      const auto* domain = static_cast<const VirglShaderModule*>(desc.domain_shader);
      if ((hull->tess_output() != domain->tess_output() || hull->tess_partitioning() != domain->tess_partitioning() || (desc.stream_output_count && !gs_)) && domain->tgsi()) {
        const uint32_t length = string_length(domain->tgsi());
        char* text = static_cast<char*>(device.gpu().allocator().allocate(length + 1));
        if (!text) return;
        __builtin_memcpy(text, domain->tgsi(), length + 1);
        auto set_digit = [&](const char* name, char digit) {
          const uint32_t n = string_length(name);
          for (uint32_t i = 0; i + n < length; ++i) {
            uint32_t k = 0;
            while (k < n && text[i + k] == name[k]) ++k;
            if (k == n) {
              text[i + n] = digit;
              return;
            }
          }
        };
        set_digit("TES_VERTEX_ORDER_CW ", hull->tess_output() == TessOutput::TriangleCw ? '1' : '0');
        set_digit("TES_POINT_MODE ", hull->tess_output() == TessOutput::Point ? '1' : '0');
        // (PIPE_TESS_SPACING: 0 fractional odd, 1 fractional even, 2 equal, which integer partitioning is and power-of-two is after the control shader rounds the factors up)
        set_digit("TES_SPACING ", hull->tess_partitioning() == TessPartitioning::FractionalOdd ? '0' : hull->tess_partitioning() == TessPartitioning::FractionalEven ? '1' : '2');
        ShaderModuleDesc variant;
        variant.stage = ShaderStage::Domain;
        variant.tgsi = text;
        variant.tess_output = hull->tess_output();
        variant.tess_partitioning = hull->tess_partitioning();
        VirglShaderModule module(device, variant, desc.stream_output_count && !gs_ ? &desc : nullptr);
        device.gpu().allocator().deallocate(text);
        if (!module.valid()) return;
        ds_ = module.handle();
      }
    }
    // A depth bias of a normalised format: the fragment shader writes the biased depth (above).
    if (desc.depth_bias.enabled() && desc.depth_bias.unorm_bits) {
      const auto* fragment = static_cast<const VirglShaderModule*>(desc.fragment_shader);
      if (!fragment->tgsi() || fragment->has_images()) return;   // (a shader made for the images of each draw has no one text to add it to: not made)
      ByteBuffer text(device.gpu().allocator());
      if (!depth_bias_fragment_text(text_view(fragment->tgsi()), desc.depth_bias, text)) return;
      ShaderModuleDesc variant;
      variant.stage = ShaderStage::Fragment;
      variant.tgsi = text_chars(text.view());
      VirglShaderModule module(device, variant);
      if (!module.valid()) return;
      fs_ = own_fs_ = module.handle();
    }
    blend_ = device.new_handle();
    dsa_ = device.new_handle();
    rasterizer_ = device.new_handle();
    elements_ = device.new_handle();
    uint32_t cmd[160];   // (the blend, depth and rasterizer objects, and up to kMaxVertexAttributes vertex elements of four words)
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
    // s0: independent blend enable, logic op enable, alpha to coverage (no dither...); s1: the logic op
    cmd[n++] = (desc.independent_blend ? 1u : 0u) | (desc.logic_op_enable ? 1u << 1 : 0u) | (desc.alpha_to_coverage && desc.multisample ? 1u << 3 : 0u);
    cmd[n++] = desc.logic_op_enable ? virgl::kLogicOps[static_cast<uint32_t>(desc.logic_op) & 0xF] : 0u;
    cmd[n++] = blend_rt;
    for (uint32_t i = 1; i < 8; ++i) cmd[n++] = desc.independent_blend && i < kMaxColorTargets ? (desc.target_blend[i - 1].custom ? render_target_blend(desc.target_blend[i - 1]) : 0xFu << 27) : 0u;

    // Depth and stencil: the front faces' stencil state, and the back faces' (the same unless the pipeline has one of their own: the host then tests the faces apart).
    const uint32_t depth_func = desc.depth_test ? static_cast<uint32_t>(desc.depth_compare) : virgl::kFuncAlways;   // (CompareOp is numbered as PIPE_FUNC)
    const bool depth_on = desc.depth_test || desc.depth_write;
    const StencilState& st = desc.stencil;
    auto stencil_word = [&](const StencilState& face) {
      return !st.enable ? 0u
                        : 1u | (static_cast<uint32_t>(face.compare) << 1) | (virgl::kStencilOps[static_cast<uint32_t>(face.fail_op)] << 4) |
                              (virgl::kStencilOps[static_cast<uint32_t>(face.pass_op)] << 7) | (virgl::kStencilOps[static_cast<uint32_t>(face.depth_fail_op)] << 10) |
                              (uint32_t{face.compare_mask} << 13) | (uint32_t{face.write_mask} << 21);
    };
    stencil_reference_ = st.enable ? st.reference : 0;
    cmd[n++] = virgl::header(virgl::kCreateObject, virgl::kObjectDsa, 5);
    cmd[n++] = dsa_;
    cmd[n++] = (depth_on ? 1u : 0u) | ((desc.depth_write ? 1u : 0u) << 1) | (depth_func << 2);
    cmd[n++] = stencil_word(st);
    cmd[n++] = stencil_word(desc.separate_back_stencil ? desc.back_stencil : st);
    cmd[n++] = 0;   // alpha reference

    // Rasterizer: depth clip (or clamp) with a 0..1 clip volume (Vulkan), scissor on, GL pixel conventions; the fill mode, and the depth bias as polygon offset (GL's unit of a float
    // depth buffer - what the host's is, Z32F_S8X24 - is Direct3D's for a float format; for a normalised one the device has the constant part in units of that format's step:
    // offset_bias_units).
    // Y_0_TOP images make window space match Vulkan's, which flips what GL calls counter-clockwise.
    const uint32_t cull = desc.cull_mode == CullMode::None ? virgl::kFaceNone
                          : desc.cull_mode == CullMode::Back ? virgl::kFaceBack : virgl::kFaceFront;
    const bool front_ccw = desc.front_face == FrontFace::CounterClockwise;
    // (the winding is the same for both kinds of image: the viewport flip and Y_0_TOP, or neither, both put the picture's top row at the same place for the rasterizer)
    cmd[n++] = virgl::header(virgl::kCreateObject, virgl::kObjectRasterizer, 9);
    cmd[n++] = rasterizer_;
    const uint32_t fill = desc.fill_mode == FillMode::Wireframe ? virgl::kPolygonLine : virgl::kPolygonFill;
    const bool offset = desc.depth_bias.enabled() && !desc.depth_bias.unorm_bits;   // (a normalised format's bias is the fragment shader's)
    cmd[n++] = (desc.depth_clip ? 1u << 1 : 0u) /*depth clip*/ | (1u << 2) /*clip half z*/ | (desc.stream_output_count && desc.rasterized_stream < 0 ? 1u << 3 : 0u) /*rasterizer discard*/ | (cull << 8) |
               (fill << 10) | (fill << 12) /*fill front, back*/ | (1u << 14) /*scissor*/ | ((front_ccw ? 1u : 0u) << 15) | (offset ? (1u << 18) | (1u << 20) : 0u) /*offset line (a wireframe triangle's), tri*/ |
               (desc.multisample ? 1u << 25 : 0u) /*multisample*/ | (1u << 29) /*half pixel center*/ | (1u << 30) /*bottom edge rule*/;
    cmd[n++] = float_bits(1.0f);  // point size
    cmd[n++] = 0;                 // sprite coord enable
    cmd[n++] = uint32_t{desc.clip_distances} << 24;   // line stipple, clip planes (the clip distances enabled)
    cmd[n++] = float_bits(1.0f);  // line width
    cmd[n++] = float_bits(offset ? static_cast<float>(desc.depth_bias.constant) : 0.0f);   // offset units
    cmd[n++] = float_bits(offset ? desc.depth_bias.slope : 0.0f);                                                         // offset scale
    cmd[n++] = float_bits(offset ? desc.depth_bias.clamp : 0.0f);                                                         // offset clamp

    // Vertex elements. Element n is the vertex shader's input n on the host, so they are laid out by location (a location nothing feeds repeats the first attribute: the
    // shader does not read it). The host keeps the instance divisor with a buffer binding, so every vertex buffer slot in use gets a binding for its per-vertex attributes and
    // another for its per-instance ones (bindings_[], which the draw fills with the slots' buffers).
    uint32_t by_location[kMaxVertexAttributes];
    uint32_t element_count = 0;
    for (uint32_t& a : by_location) a = kMaxVertexAttributes;
    for (uint32_t i = 0; i < desc.attribute_count; ++i) {
      const VertexAttribute& a = desc.attributes[i];
      if (a.location >= kMaxVertexAttributes || a.slot >= kMaxVertexBuffers || by_location[a.location] != kMaxVertexAttributes) return;
      by_location[a.location] = i;
      if (a.location + 1 > element_count) element_count = a.location + 1;
    }
    cmd[n++] = virgl::header(virgl::kCreateObject, virgl::kObjectVertexElements, element_count * 4 + 1);
    cmd[n++] = elements_;
    for (uint32_t location = 0; location < element_count; ++location) {
      const VertexAttribute& a = desc.attributes[by_location[location] != kMaxVertexAttributes ? by_location[location] : 0];
      const uint8_t key = static_cast<uint8_t>(a.slot * 2 + (a.per_instance ? 1 : 0));
      uint32_t binding = 0;
      while (binding < binding_count_ && bindings_[binding] != key) ++binding;
      if (binding == binding_count_) bindings_[binding_count_++] = key;
      cmd[n++] = a.offset;
      cmd[n++] = a.per_instance ? 1 : 0;   // instance divisor
      cmd[n++] = binding;
      cmd[n++] = virgl_vertex_format(a.format);
    }
    ok_ = n <= sizeof cmd / sizeof cmd[0] && device.send_commands(cmd, n);
  }
  ~VirglPipeline() {
    if (own_fs_) device_.destroy_object(own_fs_);
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
  // The host's vertex buffer bindings: binding n is vertex buffer slot binding_slot(n), with the stride of that slot.
  uint32_t binding_count() const { return binding_count_; }
  uint32_t binding_slot(uint32_t binding) const { return bindings_[binding] / 2; }
  uint8_t stencil_reference() const { return stencil_reference_; }
  bool uses_blend_constant() const {
    if (!desc_.blend.custom) return false;
    if (desc_.blend.uses_constant()) return true;
    if (desc_.independent_blend)
      for (const BlendState& t : desc_.target_blend)
        if (t.custom && t.uses_constant()) return true;
    return false;
  }
  bool stencil_enabled() const { return desc_.stencil.enable; }
  uint32_t vertex_shader() const { return vs_; }
  uint32_t fragment_shader() const { return fs_; }
  // A fragment shader with storage images: the host's shader is the module's variant for the images bound at the draw.
  const VirglShaderModule* fragment_module() const { return static_cast<const VirglShaderModule*>(desc_.fragment_shader); }
  uint32_t geometry_shader() const { return gs_; }   // (0: none)
  uint32_t hull_shader() const { return hs_; }
  uint32_t domain_shader() const { return ds_; }
  uint32_t stream_output_buffers() const { return so_buffers_; }   // (bit n: the declaration writes buffer n)
  uint32_t stream_of_buffer(uint32_t buffer) const { return so_stream_of_buffer_[buffer & 3]; }
  uint32_t capture_vertices() const { return capture_vertices_; }   // (0: the draw's topology says)

 private:
  VirglDevice& device_;
  GraphicsPipelineDesc desc_;
  uint32_t vs_, fs_, gs_, hs_, ds_;
  uint32_t so_buffers_ = 0, capture_vertices_ = 0;
  uint8_t so_stream_of_buffer_[kMaxStreamOutputBuffers] = {};
  uint8_t stencil_reference_ = 0;
  uint32_t blend_ = 0, dsa_ = 0, rasterizer_ = 0, elements_ = 0;
  uint32_t own_fs_ = 0;   // (a fragment shader of the pipeline's own: the depth bias of a normalised format)
  uint8_t bindings_[kMaxVertexBuffers * 2] = {};   // slot * 2 + per_instance of each host vertex buffer binding
  uint32_t binding_count_ = 0;
  bool ok_ = false;
};

}  // namespace

// --- Device --------------------------------------------------------------------

bool VirglDevice::send_commands(const uint32_t* dwords, uint32_t count, bool fence) {
  const uint64_t bytes = uint64_t{count} * 4;
  hardware::VirtioGpu& g = gpu_;
  uint8_t* request = static_cast<uint8_t*>(g.allocator().allocate(sizeof(CmdSubmit) + bytes));
  if (!request) return false;
  CmdSubmit submit{header(kCmdSubmit3d), static_cast<uint32_t>(bytes), 0};
  __builtin_memcpy(request, &submit, sizeof(submit));
  __builtin_memcpy(request + sizeof(submit), dwords, bytes);
  const bool ok = g.command(ConstByteView(request, sizeof(submit) + bytes), fence);
  g.allocator().deallocate(request);
  return ok;
}

bool VirglDevice::wait_for_gpu() {
  const uint32_t nop = 0;
  return send_commands(&nop, 1, /*fence=*/true);
}

VirglDevice::~VirglDevice() {
  for (uint32_t i = 0; i < sampler_state_count_; ++i) destroy_object(sampler_states_[i].handle);
}

// (Called from submit, which RenderNode's LockedDevice runs one at a time: the table needs no lock of its own.)
uint32_t VirglDevice::sampler_state(const SamplerDesc& sampler) {
  // (PIPE_TEX_WRAP_*: repeat 0, clamp to edge 2, clamp to border 3, mirror repeat 4, mirror clamp to edge 6; PIPE_TEX_MIPFILTER_*: nearest 0, linear 1, none 2)
  static const uint32_t kWrap[] = {virgl::kWrapRepeat, virgl::kWrapClampToEdge, 4, 3, 6};
  static const uint32_t kMip[] = {virgl::kMipFilterNone, 0, 1};
  const uint32_t u = static_cast<uint32_t>(sampler.address_u), v = static_cast<uint32_t>(sampler.address_v), border = static_cast<uint32_t>(sampler.border), mip = static_cast<uint32_t>(sampler.mip_filter);
  if (u > 4 || v > 4 || border > 2 || mip > 2) return 0;
  const bool bordered = sampler.address_u == AddressMode::ClampToBorder || sampler.address_v == AddressMode::ClampToBorder;
  // The states are kept by what they are. The levels of detail only matter with mipmaps; they are taken as they are, within the range the host's levels can have.
  auto within = [](float lod, float low, float high) { return lod != lod ? 0.0f : lod < low ? low : lod > high ? high : lod; };
  const uint32_t anisotropy = sampler.filter == Filter::Linear && sampler.max_anisotropy > 1 ? (sampler.max_anisotropy > kMaxAnisotropy ? kMaxAnisotropy : sampler.max_anisotropy) : 0u;
  SamplerState wanted;
  wanted.bits = (sampler.filter == Filter::Linear ? 1u : 0u) | (u << 1) | (v << 4) | ((bordered && !sampler.custom_border ? border : 0u) << 7) | (mip << 9) | (anisotropy << 16);
  if (sampler.mip_filter != MipFilter::None) {
    wanted.bias = within(sampler.lod_bias, -16.0f, 16.0f);
    wanted.min_lod = within(sampler.min_lod, 0.0f, 16.0f);
    wanted.max_lod = within(sampler.max_lod, 0.0f, 16.0f);
  }
  const Vec4 color = bordered ? sampler_border_value(sampler) : Vec4{0, 0, 0, 0};
  wanted.border[0] = float_bits(color.x), wanted.border[1] = float_bits(color.y), wanted.border[2] = float_bits(color.z), wanted.border[3] = float_bits(color.w);
  uint32_t similar = 0;
  for (uint32_t i = 0; i < sampler_state_count_; ++i) {
    const SamplerState& have = sampler_states_[i];
    if (have.bits != wanted.bits || __builtin_memcmp(have.border, wanted.border, sizeof have.border) != 0) continue;
    if (have.bias == wanted.bias && have.min_lod == wanted.min_lod && have.max_lod == wanted.max_lod) return have.handle;
    similar = have.handle;
  }
  // (a program with more kinds of sampler than the table holds gets one that differs in its levels of detail at most; the states are the device's until it ends)
  if (sampler_state_count_ == kSamplerStates) return similar ? similar : sampler_states_[0].handle;
  const uint32_t handle = new_handle();
  const uint32_t filter = sampler.filter == Filter::Linear ? virgl::kFilterLinear : virgl::kFilterNearest;
  // (S0: the wrap modes, filters, seamless cube maps - Direct3D always filters across the edges of a cube -, and the anisotropy in bits 20 - 24; the border colour's bits as they
  // are: the host sets it with glSamplerParameterIuiv, so an integer texture's border is its integers and a float one's its floats)
  const uint32_t s0 = kWrap[u] | (kWrap[v] << 3) | (virgl::kWrapClampToEdge << 6) /* r: the slices of a volume are clamped */ | (filter << 9) | (kMip[mip] << 11) | (filter << 13) | (1u << 19) |
                      ((anisotropy & 0x1F) << 20);
  const uint32_t cmd[] = {virgl::header(virgl::kCreateObject, virgl::kObjectSamplerState, 9), handle, s0, float_bits(wanted.bias), float_bits(wanted.min_lod),
                          float_bits(sampler.mip_filter != MipFilter::None ? wanted.max_lod : 0.0f), wanted.border[0], wanted.border[1], wanted.border[2], wanted.border[3]};
  if (!send_commands(cmd, 10)) return 0;
  wanted.handle = handle;
  sampler_states_[sampler_state_count_++] = wanted;
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
  // (mipmaps: a plain texture of 8-bit colour that is only sampled, as the kernel's own; its levels are written one by one)
  if (desc.mip_levels == 0 || desc.mip_levels > mip_level_limit(desc.extent)) return nullptr;
  if (is_compressed_format(desc.format)) {
    // (block-compressed: sampled only, of any kind, plain textures with levels; filled with write_compressed)
    const uint32_t layers = desc.kind == TextureKind::TextureCube ? 6 : desc.layers;
    if (desc.usage != kImageSampled || desc.samples != 1 || layers == 0 || layers > kMaxTextureLayers || (desc.kind != TextureKind::Texture2D && desc.mip_levels != 1) ||
        (desc.kind == TextureKind::Texture2D && desc.layers != 1) || (desc.kind == TextureKind::TextureCube && desc.extent.width != desc.extent.height))
      return nullptr;
    VirglImage* image = make<VirglImage>(gpu_.allocator(), *this, desc);
    if (image && !image->valid()) {
      destroy(gpu_.allocator(), image);
      return nullptr;
    }
    return image;
  }
  // (a storage image: sampled too, one sample, not a cube - the host's image units take it as it is kept, row 0 first)
  if ((desc.usage & kImageStorage) && (!(desc.usage & kImageSampled) || desc.samples != 1 || desc.kind == TextureKind::TextureCube || desc.format == Format::D32Float || is_srgb_format(desc.format) ||
                                       !(info().features2 & kFeature2StorageImages)))
    return nullptr;
  if (desc.mip_levels != 1 && ((desc.usage & ~kImageStorage) != kImageSampled || desc.kind != TextureKind::Texture2D || desc.samples != 1 || desc.layers != 1 ||
                               (desc.format != Format::B8G8R8A8Unorm && desc.format != Format::B8G8R8A8Srgb && desc.format != Format::R8G8B8A8Unorm && desc.format != Format::R8G8B8A8Srgb)))
    return nullptr;
  if (desc.samples != 1) {
    // (a multisampled image is an 8-bit colour image, which a shader may read with texel fetches, or a depth attachment alone: resolved)
    const bool depth = desc.format == Format::D32Float;
    if ((desc.samples != 2 && desc.samples != 4) || desc.kind != TextureKind::Texture2D || (depth ? desc.usage != kImageDepthAttachment : (desc.usage & ~kImageSampled) != kImageColorAttachment) || (!depth && desc.format != Format::B8G8R8A8Unorm && desc.format != Format::R8G8B8A8Unorm)) return nullptr;
  }
  if (desc.kind != TextureKind::Texture2D) {
    // (a volume, array or cube: sampled - and drawn into, or a storage image, with kFeature2LayeredTargets / StorageImages -, 8-bit colour or a wide format (its layers staged as such:
    // write_layer_pixels_float), one level; or a depth buffer of layers, an array or a cube)
    const uint32_t layers = desc.kind == TextureKind::TextureCube ? 6 : desc.layers;
    const bool depth_layers = desc.format == Format::D32Float && desc.usage == kImageDepthAttachment && desc.kind != TextureKind::Texture3D;
    const bool colour = (desc.usage & kImageSampled) && !(desc.usage & ~(kImageSampled | kImageColorAttachment | kImageStorage)) &&
                        (desc.format == Format::B8G8R8A8Unorm || desc.format == Format::R8G8B8A8Unorm || is_wide_format(desc.format));
    if ((!colour && !depth_layers) || layers == 0 || layers > kMaxTextureLayers || desc.mip_levels != 1 || (desc.kind == TextureKind::TextureCube && desc.extent.width != desc.extent.height)) return nullptr;
    if ((depth_layers || (desc.usage & kImageColorAttachment)) && !(info().features2 & kFeature2LayeredTargets)) return nullptr;
  } else if (desc.layers != 1) {
    return nullptr;
  }
  // A texture is not drawn into: the two kinds of image are stored in opposite row order (see VirglImage).
  if ((desc.usage & kImageSampled) && (desc.usage & (kImageDepthAttachment | kImagePresent))) return nullptr;
  if ((desc.usage & kImageSampled) && desc.format != Format::B8G8R8A8Unorm && desc.format != Format::B8G8R8A8Srgb && desc.format != Format::R8G8B8A8Unorm && desc.format != Format::R8G8B8A8Srgb && !is_wide_format(desc.format)) return nullptr;
  if (!desc.extent.width || !desc.extent.height || desc.format == Format::Undefined) return nullptr;
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
  if (m && m->handle()) destroy_object(m->handle());
  if (m) m->destroy_variants(*this);
  destroy(gpu_.allocator(), m);
}

IPipeline* VirglDevice::create_graphics_pipeline(const GraphicsPipelineDesc& desc) {
  if (desc.texture_count > kMaxTextureUnits || desc.fragment_shader == nullptr) return nullptr;
  if (!desc.vertex_shader || !desc.fragment_shader || desc.attribute_count > limits().vertex_attributes ||
      desc.varying_count > limits().varyings)
    return nullptr;
  if (desc.vertex_shader->stage() != ShaderStage::Vertex || desc.fragment_shader->stage() != ShaderStage::Fragment)
    return nullptr;
  if (desc.geometry_shader && desc.geometry_shader->stage() != ShaderStage::Geometry) return nullptr;
  if ((desc.hull_shader != nullptr) != (desc.domain_shader != nullptr) || (desc.hull_shader && (desc.hull_shader->stage() != ShaderStage::Hull || desc.domain_shader->stage() != ShaderStage::Domain))) return nullptr;
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
    // (the slots from kMaxOcclusionQueries on are the device's own: the stream output primitives a transform feedback wrote, to know how far a buffer got)
    VirglBuffer* results[kQuerySlots] = {};
    uint32_t objects[kQuerySlots] = {};
    bool used[kQuerySlots] = {};
    bool synthetic[kQuerySlots] = {};   // (answered here, not by the host)
    uint64_t synthetic_value[kQuerySlots] = {};
    uint32_t open = kMaxOcclusionQueries;   // the slot between begin and end
  } queries(*this);

  const VirglPipeline* pipeline = nullptr;
  VirglImage* color = nullptr;
  VirglImage* extra_colors[kMaxColorTargets - 1] = {};
  uint32_t target_count = 1;
  VirglImage* depth = nullptr;
  VirglBuffer* vertex_buffers[kMaxVertexBuffers] = {};
  uint64_t vertex_offsets[kMaxVertexBuffers] = {};
  const VirglImage* textures[kMaxTextureUnits] = {};
  SamplerDesc samplers[kMaxTextureUnits];
  // The storage images bound (bind_storage_image), and the host's shaders bound for them (the variants of modules that have images).
  VirglImage* images[kMaxStorageImages] = {};
  StorageImageView image_views[kMaxStorageImages];
  uint32_t bound_compute_shader = 0, bound_fragment_shader = 0;
  // The formats the images bound now are seen as (VirglShaderModule::variant); the slots with nothing bound take RGBA8.
  auto image_formats = [&](uint32_t (&out)[kMaxStorageImages]) {
    for (uint32_t k = 0; k < kMaxStorageImages; ++k) out[k] = images[k] ? images[k]->image_format(image_views[k].format) : virgl::kFormatR8G8B8A8Unorm;
  };
  // SET_SHADER_IMAGES for a stage: the slots up to the last one bound (format, access, the layers first | last << 16, the level, the resource).
  auto emit_images = [&](uint32_t stage) {
    uint32_t count = 0;
    for (uint32_t k = 0; k < kMaxStorageImages; ++k)
      if (images[k]) count = k + 1;
    if (!count) return;
    stream.command(virgl::kSetShaderImages, 0, 2 + 5 * count);
    stream.put(stage);
    stream.put(0);
    for (uint32_t k = 0; k < count; ++k) {
      const StorageImageView& v = image_views[k];
      stream.put(images[k] ? images[k]->image_format(v.format) : 0);
      stream.put(virgl::kImageAccessReadWrite);
      stream.put(images[k] ? v.first_layer | ((v.first_layer + v.layer_count - 1) << 16) : 0);
      stream.put(images[k] ? v.level : 0);
      stream.put(images[k] ? images[k]->id() : 0);
    }
  };
  bool textures_dirty = false, compute_textures_dirty = false;
  VirglBuffer* index_buffer = nullptr;
  uint64_t index_offset = 0;
  IndexType index_type = IndexType::Uint16;
  // (the viewports and scissor rectangles set: with kFeature2Viewports all of the host's sixteen are set, those past the ones given to the first - a primitive whose SV_ViewportArrayIndex
  // is past them is drawn with the first, as on the CPU renderer)
  Viewport viewports[kMaxViewports];
  Rect2D scissors[kMaxViewports];
  uint32_t viewport_count = 1, scissor_count = 1;
  const uint32_t host_viewports = (info().features2 & kFeature2Viewports) ? kMaxViewports : 1u;
  uint32_t constants[kMaxPushConstantBytes / 4] = {};
  uint32_t constants_dwords = 0;
  bool constants_dirty = false;
  bool in_pass = false;
  PrimitiveTopology topology = PrimitiveTopology::TriangleList;
  const VirglComputePipeline* compute = nullptr;
  uint32_t patch_points = 0;
  struct StorageBinding {
    VirglBuffer* buffer = nullptr;
    uint64_t offset = 0, size = 0;
  } storage[kMaxStorageBuffers];
  bool storage_dirty = false, draw_storage_dirty = false, compute_constants_dirty = false;
  // Stream output: the buffers bound (kept here) and the host's target objects for them. The objects are made when a draw with stream output first needs them and live to the end of the submit:
  // the host's transform feedback goes on in them from draw to draw (the position in the buffers is the host's) and only ends when they are destroyed; it can only go on with the program it
  // began with, so another program that draws stream output ends it (and the buffers start again where they were bound). It makes the draws of other programs fail unless the targets are not
  // set (no targets) for them. The primitives written are counted by queries of our own to know how far each buffer got, for the next bind that appends.
  struct StreamOutputSlot {
    VirglBuffer* buffer = nullptr;
    uint64_t offset = 0;
  } so_slot[kMaxStreamOutputBuffers];
  uint32_t so_handles[kMaxStreamOutputBuffers] = {};
  uint32_t so_program[5] = {};   // (the shaders of the pipeline the transform feedback began with)
  bool so_alive = false, so_set = false, disjoint_asked = false;
  uint32_t so_query_slot[kMaxGeometryStreams] = {};   // (0: none, else the slot of the Queries)
  struct Fill {
    VirglBuffer* buffer = nullptr;
    uint64_t base = 0;
    uint32_t stride = 0, vertices = 0, query = 0;   // (query 0: not counted)
  } fills[kFillQueries];
  uint32_t fill_count = 0, internal_queries = 0;
  bool user_so_query[kMaxGeometryStreams] = {};   // (a stream output statistics query of the commands: the host allows one at a time, ours stand aside)
  for (const RecordedCommand& c : recorder.commands())
    if (c.type == RecordedCommand::Type::BeginQuery && (c.count & 0xFF) == static_cast<uint32_t>(QueryKind::SoPrimitivesWritten)) user_so_query[c.count >> 8 & 3] = true;
  const ConstByteView pool = recorder.constants();

  // The buffers the commands bind (vertex, index, storage and stream output): each part of the stream that goes out tells them its work number, so that a
  // later upload into one of them knows whether the GPU may still be reading it (VirglBuffer::unmap). A submit that binds more than this has all buffers count as used.
  constexpr uint32_t kTrackedBuffers = 32;
  VirglBuffer* used[kTrackedBuffers] = {};
  uint32_t used_count = 0;
  bool used_untracked = false;
  auto use = [&](VirglBuffer* buffer) {
    if (!buffer) return;
    for (uint32_t k = 0; k < used_count; ++k)
      if (used[k] == buffer) return;
    if (used_count < kTrackedBuffers) used[used_count++] = buffer;
    else used_untracked = true;
  };

  auto flush = [&]() {
    if (stream.count() && !send_commands(stream.data(), stream.count())) return false;
    if (stream.count()) {
      const uint64_t work = gpu_.work_sent();   // (this part's number, or a later one if another thread sent meanwhile: later is safe)
      for (uint32_t k = 0; k < used_count; ++k) used[k]->note_use(work);
      if (used_untracked) {
        uint64_t seen = __atomic_load_n(&all_used_until_, __ATOMIC_RELAXED);
        while (work > seen && !__atomic_compare_exchange_n(&all_used_until_, &seen, work, true, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {}
      }
    }
    stream.reset();
    return true;
  };

  auto unset_stream_output = [&]() {
    if (!so_set) return;
    stream.command(virgl::kSetStreamoutTargets, 0, 1);
    stream.put(0);   // (no targets)
    so_set = false;
  };
  // Ends the transform feedback: its queries, and the target objects.
  auto end_stream_output = [&]() {
    if (!so_alive) return;
    unset_stream_output();
    for (uint32_t k = 0; k < kMaxGeometryStreams; ++k) {
      if (!so_query_slot[k]) continue;
      stream.command(virgl::kEndQuery, 0, 1);
      stream.put(queries.objects[so_query_slot[k]]);
      so_query_slot[k] = 0;
    }
    for (uint32_t& handle : so_handles) {
      if (!handle) continue;
      stream.command(virgl::kDestroyObject, 0, 1);
      stream.put(handle);
      handle = 0;
    }
    so_alive = false;
  };
  // Makes the targets current for a draw of `pipeline`.
  auto begin_stream_output = [&]() -> bool {
    const uint32_t program[5] = {pipeline->vertex_shader(), pipeline->geometry_shader(), pipeline->hull_shader(), pipeline->domain_shader(), pipeline->fragment_shader()};
    bool same = true;
    for (uint32_t k = 0; k < 5; ++k) same = same && program[k] == so_program[k];
    if (so_alive && !same) end_stream_output();
    if (!so_alive) {
      const GraphicsPipelineDesc& pd = pipeline->desc();
      uint32_t vertices = pipeline->capture_vertices();
      if (!pipeline->geometry_shader() && !pipeline->domain_shader()) vertices = topology == PrimitiveTopology::TriangleList ? 3 : topology == PrimitiveTopology::LineList ? 2 : 1;
      for (uint32_t k = 0; k < kMaxStreamOutputBuffers; ++k) {
        if (!so_slot[k].buffer) continue;
        so_handles[k] = new_handle();
        stream.command(virgl::kCreateObject, virgl::kObjectStreamoutTarget, 4);
        stream.put(so_handles[k]), stream.put(so_slot[k].buffer->id()), stream.put(static_cast<uint32_t>(so_slot[k].offset)), stream.put(static_cast<uint32_t>(so_slot[k].buffer->size() - so_slot[k].offset));
      }
      // The primitives a stream wrote, for each stream the declaration takes buffers from (unless the commands have a query of that kind, which the host cannot run twice at once).
      for (uint32_t b = 0; b < kMaxStreamOutputBuffers; ++b) {
        if (!so_slot[b].buffer || !(pipeline->stream_output_buffers() >> b & 1)) continue;
        if (fill_count >= kFillQueries) return false;
        Fill& fill = fills[fill_count++];
        fill = {so_slot[b].buffer, so_slot[b].offset, pd.stream_output_stride[b], vertices, 0};
        const uint32_t output_stream = pipeline->stream_of_buffer(b);
        if (user_so_query[output_stream]) continue;
        if (!so_query_slot[output_stream]) {
          if (internal_queries >= kFillQueries) return false;
          const uint32_t slot = kMaxOcclusionQueries + internal_queries++;
          queries.results[slot] = make<VirglBuffer>(gpu_.allocator(), *this, 16, 0, virgl::kBindCustom);
          if (!queries.results[slot] || !queries.results[slot]->valid()) return false;
          queries.objects[slot] = new_handle();
          queries.used[slot] = true;
          stream.command(virgl::kCreateObject, virgl::kObjectQuery, 4);
          stream.put(queries.objects[slot]);
          stream.put(virgl::kQueryPrimitivesEmitted | (output_stream << 16));
          stream.put(0);
          stream.put(queries.results[slot]->id());
          stream.command(virgl::kBeginQuery, 0, 1);
          stream.put(queries.objects[slot]);
          so_query_slot[output_stream] = slot;
        }
        fill.query = so_query_slot[output_stream];
      }
      for (uint32_t k = 0; k < 5; ++k) so_program[k] = program[k];
      so_alive = true;
    }
    if (!so_set) {
      stream.command(virgl::kSetStreamoutTargets, 0, 5);
      stream.put(0);   // (no appending: the host's objects go on where they were)
      for (uint32_t k = 0; k < kMaxStreamOutputBuffers; ++k) stream.put(so_handles[k]);
      so_set = true;
    }
    return true;
  };
  auto emit_viewport = [&]() {
    if (!stream.command(virgl::kSetViewportState, 0, 1 + 6 * host_viewports)) return;
    stream.put(0);
    for (uint32_t i = 0; i < host_viewports; ++i) {
      const Viewport& viewport = viewports[i < viewport_count ? i : 0];
      stream.putf(viewport.width * 0.5f);
      stream.putf((color && !color->y0_top() ? 1.0f : -1.0f) * viewport.height * 0.5f);  // GL's +Y is up: flip to Vulkan's +Y down (an upright image has its row 0 at window y 0 already)
      stream.putf(viewport.max_depth - viewport.min_depth);
      stream.putf(viewport.x + viewport.width * 0.5f);
      stream.putf(viewport.y + viewport.height * 0.5f);
      stream.putf(viewport.min_depth);
    }
  };
  auto emit_scissor = [&]() {
    if (!stream.command(virgl::kSetScissorState, 0, 1 + 2 * host_viewports)) return;
    stream.put(0);
    // Scissor rectangles are counted from the bottom of the target in virgl (GL's window space), while Vulkan's - and ours - count from the top.
    const uint32_t height = color ? color->extent().height : 0;
    const bool upright = color && !color->y0_top();
    for (uint32_t i = 0; i < host_viewports; ++i) {
      const Rect2D& scissor = scissors[i < scissor_count && i < viewport_count ? i : 0];
      const uint32_t y0 = upright ? scissor.y : height - (scissor.y + scissor.height), y1 = upright ? scissor.y + scissor.height : height - scissor.y;
      stream.put(scissor.x | (y0 << 16));
      stream.put((scissor.x + scissor.width) | (y1 << 16));
    }
  };
  auto clamped_scissor = [&](const Rect2D& r) {
    const Extent2D e = color->extent();
    const int64_t x0 = r.x > 0 ? r.x : 0, y0 = r.y > 0 ? r.y : 0;
    int64_t x1 = int64_t{r.x} + r.width, y1 = int64_t{r.y} + r.height;
    if (x1 > e.width) x1 = e.width;
    if (y1 > e.height) y1 = e.height;
    return Rect2D{static_cast<int32_t>(x0), static_cast<int32_t>(y0), x1 > x0 ? static_cast<uint32_t>(x1 - x0) : 0, y1 > y0 ? static_cast<uint32_t>(y1 - y0) : 0};
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
        // (the surfaces of the layers the pass draws into: a layered one takes the primitives' gl_Layer)
        uint32_t surfaces[kMaxColorTargets + 1] = {};
        for (uint32_t n = 0; n <= kMaxColorTargets; ++n) {
          VirglImage* image = n == kMaxColorTargets ? depth : n == 0 ? color : n < target_count ? extra_colors[n - 1] : nullptr;
          if (!image) continue;
          surfaces[n] = cmd.pass.first_layer[n] == 0 && cmd.pass.layer_count[n] == 1 ? image->surface() : image->surface(cmd.pass.first_layer[n], cmd.pass.layer_count[n]);
          if (!surfaces[n]) return false;
        }
        stream.command(virgl::kSetFramebufferState, 0, 2 + target_count);
        stream.put(target_count);
        stream.put(surfaces[kMaxColorTargets]);
        for (uint32_t n = 0; n < target_count; ++n) stream.put(surfaces[n]);
        viewports[0] = {0, 0, static_cast<float>(e.width), static_cast<float>(e.height), 0, 1};
        scissors[0] = {0, 0, e.width, e.height};
        viewport_count = scissor_count = 1;
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
      case RecordedCommand::Type::SetPatchPoints:
        patch_points = cmd.count;
        break;
      case RecordedCommand::Type::EndRenderPass:
        if (queries.open != kMaxOcclusionQueries) return false;
        unset_stream_output();
        in_pass = false;
        break;
      case RecordedCommand::Type::Resolve: {
        // A blit from the multisampled image to the plain one; one kept upright (an image that is only drawn into) is turned over.
        const VirglImage* to = static_cast<const VirglImage*>(cmd.image);
        const VirglImage* from = static_cast<const VirglImage*>(cmd.image2);
        if (in_pass || !to || !from || to->samples() != 1 || from->samples() < 2 || to->extent().width != from->extent().width || to->extent().height != from->extent().height) return false;
        const Extent2D e = from->extent();
        const bool depth = to->format() == Format::D32Float;
        // (the host's blit averages colour and takes the first sample of depth: the other modes are not made)
        if (static_cast<ResolveMode>(cmd.count) != (depth ? ResolveMode::Sample0 : ResolveMode::Average)) return false;
        const bool flip = !depth && to->y0_top();   // (a multisampled image does not take the flag: the host stores it the way GL does, and only a destination kept upright needs the blit to turn it over)
        stream.command(virgl::kBlit, 0, 21);
        stream.put(depth ? (to->has_stencil() ? 0x30u : 0x10u) : 0xFu);   // (all four channels, or depth and stencil; nearest, no scissor)
        stream.put(0);
        stream.put(e.width | (e.height << 16));
        stream.put(to->id()), stream.put(0), stream.put(to->blit_format());
        stream.put(0), stream.put(flip ? e.height : 0), stream.put(0), stream.put(e.width), stream.put(flip ? static_cast<uint32_t>(-static_cast<int32_t>(e.height)) : e.height), stream.put(1);
        stream.put(from->id()), stream.put(0), stream.put(from->blit_format());
        stream.put(0), stream.put(0), stream.put(0), stream.put(e.width), stream.put(e.height), stream.put(1);
        break;
      }
      case RecordedCommand::Type::BindComputePipeline: {
        compute = static_cast<const VirglComputePipeline*>(cmd.pipeline);
        if (in_pass || !compute) return false;
        stream.command(virgl::kBindShader, 0, 2);
        stream.put(compute->shader());
        stream.put(virgl::kShaderCompute);
        bound_compute_shader = compute->shader();   // (0 for a shader with images: its variant goes with the dispatch)
        compute_constants_dirty = constants_dwords != 0;
        break;
      }
      case RecordedCommand::Type::BindStreamOutput: {
        if (cmd.unit >= kMaxStreamOutputBuffers) return false;
        VirglBuffer* target = static_cast<VirglBuffer*>(cmd.buffer);
        const bool append = cmd.offset == kStreamOutputAppend;
        if (append && target && so_slot[cmd.unit].buffer == target) break;   // (it goes on where it is)
        uint64_t start = cmd.offset;
        if (append && target) {
          // (how far the buffer got in an earlier submit; one written in this one has no known end until the host has run it)
          for (uint32_t f = 0; f < fill_count; ++f)
            if (fills[f].buffer == target) return false;
          start = target->stream_output_filled();
          if (start == kStreamOutputAppend) return false;
        }
        if (target && start > target->size()) return false;
        end_stream_output();   // (the draws that follow start at this binding's beginning)
        so_slot[cmd.unit] = {target, start};
        use(target);
        break;
      }
      case RecordedCommand::Type::BindStorageImage: {
        if (cmd.unit >= kMaxStorageImages) return false;
        VirglImage* image = static_cast<VirglImage*>(cmd.image);
        const StorageImageView& v = cmd.storage_view;
        if (image && (!(image->usage() & kImageStorage) || v.level >= image->mip_levels() || v.layer_count == 0 || v.first_layer + v.layer_count > image->layer_count() ||
                      !storage_view_compatible(image->format(), v.format)))
          return false;
        images[cmd.unit] = image;
        image_views[cmd.unit] = v;
        break;
      }
      case RecordedCommand::Type::BindStorageBuffer:
        if (cmd.unit >= kMaxStorageBuffers || !cmd.buffer) return false;   // (the bindings may change between draws)
        storage[cmd.unit] = {static_cast<VirglBuffer*>(cmd.buffer), cmd.offset, cmd.count};
        use(storage[cmd.unit].buffer);
        storage_dirty = true;
        draw_storage_dirty = true;
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
        if (compute_textures_dirty) {
          // The textures bound (a prefix of the units, up to the last one bound: the kernel has checked that those the shader reads are there), for the compute stage.
          uint32_t unit_count = 0;
          for (uint32_t unit = 0; unit < kMaxTextureUnits; ++unit)
            if (textures[unit]) unit_count = unit + 1;
          if (unit_count) {
            uint32_t states[kMaxTextureUnits] = {};
            for (uint32_t unit = 0; unit < unit_count; ++unit)
              if (textures[unit] && !(states[unit] = sampler_state(samplers[unit]))) return false;
            stream.command(virgl::kSetSamplerViews, 0, 2 + unit_count);
            stream.put(virgl::kShaderCompute);
            stream.put(0);
            for (uint32_t unit = 0; unit < unit_count; ++unit) stream.put(textures[unit] ? textures[unit]->view() : 0);
            stream.command(virgl::kBindSamplerStates, 0, 2 + unit_count);
            stream.put(virgl::kShaderCompute);
            stream.put(0);
            for (uint32_t unit = 0; unit < unit_count; ++unit) stream.put(states[unit]);
          }
          compute_textures_dirty = false;
        }
        if (compute_constants_dirty) {
          stream.command(virgl::kSetConstantBuffer, 0, 2 + constants_dwords);
          stream.put(virgl::kShaderCompute);
          stream.put(0);
          for (uint32_t k = 0; k < constants_dwords; ++k) stream.put(constants[k]);
          compute_constants_dirty = false;
        }
        if (compute->module()->has_images()) {
          // (the host's shader for the formats of the images bound, and the images)
          uint32_t formats[kMaxStorageImages];
          image_formats(formats);
          const uint32_t shader = compute->module()->variant(*this, formats);
          if (!shader) return false;
          if (shader != bound_compute_shader) {
            stream.command(virgl::kBindShader, 0, 2);
            stream.put(shader);
            stream.put(virgl::kShaderCompute);
            bound_compute_shader = shader;
          }
          emit_images(virgl::kShaderCompute);
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
        if (!in_pass || queries.open != kMaxOcclusionQueries || cmd.unit >= kMaxOcclusionQueries || queries.used[cmd.unit] || (cmd.count & 0xFF) > static_cast<uint32_t>(QueryKind::TimestampDisjoint)) return false;
        if ((cmd.count & 0xFF) >= static_cast<uint32_t>(QueryKind::TimestampFrequency)) {
          // (the host has no such query: its timestamps are nanoseconds and it cannot say whether the clock was interrupted)
          queries.used[cmd.unit] = queries.synthetic[cmd.unit] = true;
          queries.synthetic_value[cmd.unit] = (cmd.count & 0xFF) == static_cast<uint32_t>(QueryKind::TimestampFrequency) ? 1000000000u : (clock_interrupted_ ? 1u : 0u);
          disjoint_asked = disjoint_asked || (cmd.count & 0xFF) == static_cast<uint32_t>(QueryKind::TimestampDisjoint);
          queries.open = cmd.unit;
          break;
        }
        queries.results[cmd.unit] = make<VirglBuffer>(gpu_.allocator(), *this, 16, 0, virgl::kBindCustom);
        if (!queries.results[cmd.unit] || !queries.results[cmd.unit]->valid()) return false;
        const uint32_t handle = new_handle();
        stream.command(virgl::kCreateObject, virgl::kObjectQuery, 4);
        stream.put(handle);
        {
          const QueryKind kind = static_cast<QueryKind>(cmd.count & 0xFF);
          const uint32_t output_stream = cmd.count >> 8 & 3;
          stream.put(kind == QueryKind::Occlusion ? virgl::kQueryOcclusionCounter
                     : kind == QueryKind::OcclusionPredicate ? virgl::kQueryOcclusionPredicate
                     : kind == QueryKind::Timestamp ? virgl::kQueryTimestamp
                     : kind == QueryKind::SoPrimitivesWritten ? virgl::kQueryPrimitivesEmitted | (output_stream << 16)
                     : kind == QueryKind::SoPrimitivesNeeded ? virgl::kQueryPrimitivesGenerated | (output_stream << 16)
                                                             : virgl::kQueryPipelineStatistics | ((kind == QueryKind::IaVertices ? 0u : kind == QueryKind::IaPrimitives ? 1u : kind == QueryKind::VsInvocations ? 2u : 7u) << 16));
        }
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
        if (!queries.synthetic[queries.open]) {
          stream.command(virgl::kEndQuery, 0, 1);
          stream.put(queries.objects[queries.open]);
        }
        queries.open = kMaxOcclusionQueries;
        break;
      case RecordedCommand::Type::BindPipeline: {
        pipeline = static_cast<const VirglPipeline*>(cmd.pipeline);
        if (!pipeline) return false;
        if (!pipeline->desc().stream_output_count) unset_stream_output();
        stream.command(virgl::kBindObject, virgl::kObjectBlend, 1);
        stream.put(pipeline->blend());
        stream.command(virgl::kBindObject, virgl::kObjectDsa, 1);
        stream.put(pipeline->dsa());
        stream.command(virgl::kBindObject, virgl::kObjectRasterizer, 1);
        stream.put(pipeline->rasterizer());
        if (pipeline->uses_blend_constant()) {
          // (the colour of the constant blend factors: one for all the targets, as in Direct3D and OpenGL)
          stream.command(virgl::kSetBlendColor, 0, 4);
          for (const float channel : pipeline->desc().blend.constant) stream.putf(channel);
        }
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
        bound_fragment_shader = pipeline->fragment_shader();
        // (always: the host keeps a geometry shader bound, across submits too, until another bind - none, handle 0, for a pipeline without one)
        stream.command(virgl::kBindShader, 0, 2);
        stream.put(pipeline->geometry_shader());
        stream.put(virgl::kShaderGeometry);
        stream.command(virgl::kBindShader, 0, 2);
        stream.put(pipeline->hull_shader());
        stream.put(virgl::kShaderTessControl);
        stream.command(virgl::kBindShader, 0, 2);
        stream.put(pipeline->domain_shader());
        stream.put(virgl::kShaderTessEval);
        constants_dirty = constants_dwords != 0;  // constants belong to the program: resend
        break;
      }
      case RecordedCommand::Type::BindVertexBuffer:
        if (cmd.unit >= kMaxVertexBuffers) return false;
        vertex_buffers[cmd.unit] = static_cast<VirglBuffer*>(cmd.buffer);
        vertex_offsets[cmd.unit] = cmd.offset;
        use(vertex_buffers[cmd.unit]);
        break;
      case RecordedCommand::Type::BindIndexBuffer:
        index_buffer = static_cast<VirglBuffer*>(cmd.buffer);
        index_offset = cmd.offset;
        use(index_buffer);
        index_type = cmd.index_type;
        break;
      case RecordedCommand::Type::BindTexture: {
        const VirglImage* image = static_cast<const VirglImage*>(cmd.image);
        if (cmd.unit >= kMaxTextureUnits || !image || !image->view()) return false;   // (only an image created with kImageSampled has a view)
        textures[cmd.unit] = image;
        samplers[cmd.unit] = cmd.sampler;
        textures_dirty = true;
        compute_textures_dirty = true;
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
        viewports[0] = cmd.viewport;
        viewport_count = 1;
        emit_viewport();
        emit_scissor();   // (the scissors past the first follow the viewports' count)
        break;
      case RecordedCommand::Type::SetViewportAt:
        if (cmd.unit >= kMaxViewports || cmd.count == 0 || cmd.count > kMaxViewports || (cmd.count > 1 && host_viewports < kMaxViewports)) return false;
        viewports[cmd.unit] = cmd.viewport;
        viewport_count = cmd.count;
        if (cmd.unit + 1 == cmd.count) {   // (the last of them: the host gets them all at once)
          emit_viewport();
          emit_scissor();
        }
        break;
      case RecordedCommand::Type::SetScissor: {
        if (!in_pass) return false;
        scissors[0] = clamped_scissor(cmd.scissor);
        scissor_count = 1;
        emit_scissor();
        break;
      }
      case RecordedCommand::Type::SetScissorAt: {
        if (!in_pass || cmd.unit >= kMaxViewports || cmd.count == 0 || cmd.count > kMaxViewports || (cmd.count > 1 && host_viewports < kMaxViewports)) return false;
        scissors[cmd.unit] = clamped_scissor(cmd.scissor);
        scissor_count = cmd.count;
        if (cmd.unit + 1 == cmd.count) emit_scissor();
        break;
      }
      case RecordedCommand::Type::Draw:
      case RecordedCommand::Type::DrawIndexed: {
        if (!in_pass || !pipeline || pipeline->desc().color_target_count > target_count) return false;
        for (uint32_t binding = 0; binding < pipeline->binding_count(); ++binding)
          if (!vertex_buffers[pipeline->binding_slot(binding)]) return false;   // (a slot the pipeline reads with no buffer)
        const bool indexed = cmd.type == RecordedCommand::Type::DrawIndexed;
        if (indexed && !index_buffer) return false;
        if (pipeline->desc().stream_output_count && !begin_stream_output()) return false;
        if (draw_storage_dirty) {
          // The buffers the stages of a draw read and write, from the host's binding point 0: the views first (t0 .. t3), the unordered access buffers after them (u0 .. u3), the way their TGSI
          // numbers them; the compute shader's bindings are made again before a dispatch.
          StorageBinding host[kMaxStorageBuffers];
          uint32_t slots = 0;
          for (uint32_t k = 0; k < kMaxStorageBuffers; ++k) {
            host[k] = storage[k < kMaxSrvBuffers ? kMaxUavBuffers + k : k - kMaxSrvBuffers];
            if (host[k].buffer) slots = k + 1;
          }
          const GraphicsPipelineDesc& pd = pipeline->desc();
          const uint32_t stages[5] = {virgl::kShaderVertex, virgl::kShaderFragment, virgl::kShaderGeometry, virgl::kShaderTessControl, virgl::kShaderTessEval};
          for (uint32_t which = 0; which < 5; ++which) {
            if ((which == 2 && !pd.geometry_shader) || ((which == 3 || which == 4) && !pd.hull_shader)) continue;   // (only the stages the pipeline has)
            const uint32_t stage = stages[which];
            stream.command(virgl::kSetShaderBuffers, 0, 2 + 3 * slots);
            stream.put(stage);
            stream.put(0);
            for (uint32_t k = 0; k < slots; ++k) {
              stream.put(static_cast<uint32_t>(host[k].offset));
              stream.put(static_cast<uint32_t>(host[k].size));
              stream.put(host[k].buffer ? host[k].buffer->id() : 0);
            }
          }
          storage_dirty = true;
          draw_storage_dirty = false;
        }
        if (const uint32_t unit_count = pipeline->desc().texture_count) {
          for (uint32_t unit = 0; unit < unit_count; ++unit)
            if (!textures[unit] || textures[unit] == color || textures[unit] == depth) return false;   // unbound, or the image being drawn into
          for (uint32_t unit = 0; unit < unit_count; ++unit)
            for (const VirglImage* extra : extra_colors)
              if (textures[unit] == extra) return false;
          if (textures_dirty) {
            // (to the vertex and the pixel stage alike: a vertex shader may read the units too, numbered as the pipeline's)
            uint32_t states[kMaxTextureUnits];
            for (uint32_t unit = 0; unit < unit_count; ++unit) {
              states[unit] = sampler_state(samplers[unit]);   // (created straight away, so it exists before the stream that uses it)
              if (!states[unit]) return false;
            }
            // (and to the geometry, control and evaluation stages the pipeline has: a texture read there, by the same numbers)
            for (const uint32_t stage : {virgl::kShaderVertex, virgl::kShaderFragment, virgl::kShaderGeometry, virgl::kShaderTessControl, virgl::kShaderTessEval}) {
              if ((stage == virgl::kShaderGeometry && !pipeline->geometry_shader()) || (stage == virgl::kShaderTessControl && !pipeline->hull_shader()) ||
                  (stage == virgl::kShaderTessEval && !pipeline->domain_shader()))
                continue;
              stream.command(virgl::kSetSamplerViews, 0, 2 + unit_count);
              stream.put(stage);
              stream.put(0);
              for (uint32_t unit = 0; unit < unit_count; ++unit) stream.put(textures[unit]->view());
              stream.command(virgl::kBindSamplerStates, 0, 2 + unit_count);
              stream.put(stage);
              stream.put(0);
              for (uint32_t unit = 0; unit < unit_count; ++unit) stream.put(states[unit]);
            }
            textures_dirty = false;
          }
        }
        if (constants_dirty) {
          // (the vertex and fragment shaders' stages, and those of the geometry, hull and domain shaders the pipeline has: shader types 2, 3 and 4)
          for (uint32_t shader = 0; shader < 5; ++shader) {
            if ((shader == 2 && !pipeline->geometry_shader()) || (shader == 3 && !pipeline->hull_shader()) || (shader == 4 && !pipeline->domain_shader())) continue;
            stream.command(virgl::kSetConstantBuffer, 0, 2 + constants_dwords);
            stream.put(shader);
            stream.put(0);
            for (uint32_t k = 0; k < constants_dwords; ++k) stream.put(constants[k]);
          }
          constants_dirty = false;
        }
        if (const uint32_t bindings = pipeline->binding_count()) {
          stream.command(virgl::kSetVertexBuffers, 0, bindings * 3);
          for (uint32_t binding = 0; binding < bindings; ++binding) {
            const uint32_t slot = pipeline->binding_slot(binding);
            stream.put(slot == 0 ? pipeline->desc().vertex_stride : pipeline->desc().slot_strides[slot]);
            stream.put(static_cast<uint32_t>(vertex_offsets[slot]));
            stream.put(vertex_buffers[slot]->id());
          }
        }
        if (indexed) {
          stream.command(virgl::kSetIndexBuffer, 0, 3);
          stream.put(index_buffer->id());
          stream.put(index_type == IndexType::Uint16 ? 2 : 4);
          // (the host draws from the index buffer's offset and does not look at the draw's first index: the first index is part of the offset)
          stream.put(static_cast<uint32_t>(index_offset + uint64_t{cmd.first} * (index_type == IndexType::Uint16 ? 2 : 4)));
        }
        if (pipeline->fragment_module()->has_images()) {
          // (a pixel shader with storage images: its host shader for their formats, and the images)
          uint32_t formats[kMaxStorageImages];
          image_formats(formats);
          const uint32_t shader = pipeline->fragment_module()->variant(*this, formats);
          if (!shader) return false;
          if (shader != bound_fragment_shader) {
            stream.command(virgl::kBindShader, 0, 2);
            stream.put(shader);
            stream.put(virgl::kShaderFragment);
            bound_fragment_shader = shader;
          }
          emit_images(virgl::kShaderFragment);
        }
        const bool patches = topology == PrimitiveTopology::PatchList;
        stream.command(virgl::kDrawVbo, 0, patches ? 14 : 12);
        stream.put(indexed ? 0 : cmd.first);
        stream.put(cmd.count);
        stream.put(patches ? virgl::kPrimPatches : topology == PrimitiveTopology::LineListAdjacency ? virgl::kPrimLinesAdjacency : topology == PrimitiveTopology::TriangleListAdjacency ? virgl::kPrimTrianglesAdjacency : topology == PrimitiveTopology::LineList ? virgl::kPrimLines : topology == PrimitiveTopology::PointList ? virgl::kPrimPoints : virgl::kPrimTriangles);
        stream.put(indexed ? 1 : 0);
        stream.put(cmd.instance_count);
        stream.put(static_cast<uint32_t>(cmd.vertex_offset));
        stream.put(cmd.first_instance);
        stream.put(0);  // primitive restart
        stream.put(0);  // restart index
        stream.put(0);  // min index
        stream.put(0xFFFFFFFFu);  // max index
        stream.put(0);  // count from stream output
        if (patches) {
          stream.put(patch_points);
          stream.put(0);   // draw id
        }
        if (pipeline->desc().fragment_stores || pipeline->desc().vertex_stores || pipeline->desc().geometry_stores || pipeline->desc().tessellation_stores) {   // (what the shaders wrote is there for the next read, a transfer to the system included)
          stream.command(virgl::kMemoryBarrier, 0, 1);
          stream.put(0xFFFFFFFFu);
        }
        break;
      }
    }
    if (stream.overflow()) return false;
    if (stream.count() > Stream::kCapacity - 512 && !flush()) return false;  // leave room for the next command
  }
  end_stream_output();
  if (in_pass || !flush()) return false;
  // Ask the host for each query's count (waiting for the draws), then read what it wrote into the buffer.
  bool asked = false;
  for (uint32_t slot = 0; slot < kQuerySlots; ++slot) {
    if (!queries.used[slot] || queries.synthetic[slot]) continue;
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
    for (uint32_t slot = 0; slot < kQuerySlots; ++slot) {
      if (!queries.used[slot] || queries.synthetic[slot]) continue;
      ByteReader in(queries.results[slot]->map());
      if (in.le32() != virgl::kQueryStateDone) pending = true;
    }
    if (!pending) break;
    const uint32_t nop = 0;
    if (attempt > kQueryPolls || !send_commands(&nop, 1, /*fence=*/true)) return false;   // (a fenced one: the host looks at its queries while it waits for fences)
  }
  if (disjoint_asked) clock_interrupted_ = false;
  uint64_t counts[kMaxOcclusionQueries + kFillQueries] = {};
  for (uint32_t slot = 0; slot < kQuerySlots; ++slot) {
    if (!queries.used[slot]) continue;
    if (queries.synthetic[slot]) {
      counts[slot] = queries.synthetic_value[slot];
      recorder.set_occlusion_result(slot, counts[slot]);
      continue;
    }
    ByteReader in(queries.results[slot]->map());
    in.le32();   // (the state, DONE)
    in.le32();   // (the size of the result: 4 for a sample count, 8 for a time)
    const uint64_t low = in.le32();
    const uint64_t high = in.le32();
    if (!in.ok()) return false;
    counts[slot] = low | (high << 32);
    if (slot < kMaxOcclusionQueries) recorder.set_occlusion_result(slot, counts[slot]);
  }
  // How far each stream output buffer got (the last binding of a buffer in the commands counts; where the commands had a query of the kind that ours stand aside for, nobody knows).
  for (uint32_t f = 0; f < fill_count; ++f) {
    const Fill& fill = fills[f];
    const uint64_t bytes = fill.query ? counts[fill.query] * fill.vertices * fill.stride : 0;
    const uint64_t end = fill.query && bytes <= fill.buffer->size() - fill.base ? fill.base + bytes : kStreamOutputAppend;
    fill.buffer->set_stream_output_filled(end);
  }
  return true;
}

bool VirglDevice::present(IImage* image) {
  auto* img = static_cast<VirglImage*>(image);
  if (!img || !(img->usage() & kImagePresent)) return false;
  return gpu_.show_resource(img->id(), img->extent().width, img->extent().height);
}

}  // namespace graphics
