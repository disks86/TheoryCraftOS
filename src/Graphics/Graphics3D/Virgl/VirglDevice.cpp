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
#include "Graphics/Graphics3D/Virgl/VirglVertexPatch.hpp"
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
// The host's query targets: what one host query counts. The host runs one query of a target at a time (GL's rule), so each of ours is made of segments of the targets it needs.
constexpr uint32_t kTargetOcclusion = 0, kTargetStatistic = 1, kTargetWritten = kTargetStatistic + kPipelineStatisticCount, kTargetNeeded = kTargetWritten + kMaxGeometryStreams,
                   kQueryTargets = kTargetNeeded + kMaxGeometryStreams;
uint32_t host_query_type(uint32_t target) {
  if (target == kTargetOcclusion) return virgl::kQueryOcclusionCounter;
  if (target < kTargetWritten) return virgl::kQueryPipelineStatistics | ((target - kTargetStatistic) << 16);
  if (target < kTargetNeeded) return virgl::kQueryPrimitivesEmitted | ((target - kTargetWritten) << 16);
  return virgl::kQueryPrimitivesGenerated | ((target - kTargetNeeded) << 16);
}
// The targets a query of ours needs (bit n: target n); 0 for one the host does not count (timestamps are a host query of their own at the end, the frequency and disjoint answered here).
uint32_t query_targets(QueryKind kind, uint32_t stream) {
  auto statistic = [](PipelineStatistic s) { return 1u << (kTargetStatistic + static_cast<uint32_t>(s)); };
  switch (kind) {
    case QueryKind::Occlusion: case QueryKind::OcclusionPredicate: return 1u << kTargetOcclusion;
    case QueryKind::IaVertices: return statistic(PipelineStatistic::IaVertices);
    case QueryKind::IaPrimitives: return statistic(PipelineStatistic::IaPrimitives);
    case QueryKind::VsInvocations: return statistic(PipelineStatistic::VsInvocations);
    case QueryKind::PsInvocations: return statistic(PipelineStatistic::PsInvocations);
    case QueryKind::SoPrimitivesWritten: return 1u << (kTargetWritten + stream);
    case QueryKind::SoPrimitivesNeeded: return 1u << (kTargetNeeded + stream);
    case QueryKind::PipelineStatistics: return ((1u << kPipelineStatisticCount) - 1) << kTargetStatistic;
    default: return 0;
  }
}
constexpr uint32_t kQueryPolls = 5000;   // how many empty commands to wait for the host's query results with

// --- Objects -------------------------------------------------------------------

// The host's formats of the typed views of buffers (TexelFormat, DXGI's numbers): VIRGL_FORMAT_* (virglrenderer's virgl_hw.h) and Gallium's name of it, which the TGSI parser takes.
struct TexelHostFormat {
  TexelFormat format;
  uint32_t virgl;
  const char* name;
};
constexpr TexelHostFormat kTexelHostFormats[] = {
    {TexelFormat::R32G32B32A32Float, 31, "PIPE_FORMAT_R32G32B32A32_FLOAT"}, {TexelFormat::R32G32B32A32Uint, 196, "PIPE_FORMAT_R32G32B32A32_UINT"},
    {TexelFormat::R32G32B32A32Sint, 200, "PIPE_FORMAT_R32G32B32A32_SINT"}, {TexelFormat::R32G32B32Float, 30, "PIPE_FORMAT_R32G32B32_FLOAT"},
    {TexelFormat::R32G32B32Uint, 195, "PIPE_FORMAT_R32G32B32_UINT"}, {TexelFormat::R32G32B32Sint, 199, "PIPE_FORMAT_R32G32B32_SINT"},
    {TexelFormat::R16G16B16A16Float, 94, "PIPE_FORMAT_R16G16B16A16_FLOAT"}, {TexelFormat::R16G16B16A16Unorm, 51, "PIPE_FORMAT_R16G16B16A16_UNORM"},
    {TexelFormat::R16G16B16A16Uint, 188, "PIPE_FORMAT_R16G16B16A16_UINT"}, {TexelFormat::R16G16B16A16Snorm, 59, "PIPE_FORMAT_R16G16B16A16_SNORM"},
    {TexelFormat::R16G16B16A16Sint, 192, "PIPE_FORMAT_R16G16B16A16_SINT"}, {TexelFormat::R32G32Float, 29, "PIPE_FORMAT_R32G32_FLOAT"},
    {TexelFormat::R32G32Uint, 194, "PIPE_FORMAT_R32G32_UINT"}, {TexelFormat::R32G32Sint, 198, "PIPE_FORMAT_R32G32_SINT"},
    {TexelFormat::R10G10B10A2Unorm, 8, "PIPE_FORMAT_R10G10B10A2_UNORM"}, {TexelFormat::R10G10B10A2Uint, 253, "PIPE_FORMAT_R10G10B10A2_UINT"},
    {TexelFormat::R11G11B10Float, 124, "PIPE_FORMAT_R11G11B10_FLOAT"}, {TexelFormat::R8G8B8A8Unorm, 67, "PIPE_FORMAT_R8G8B8A8_UNORM"},
    {TexelFormat::R8G8B8A8Uint, 180, "PIPE_FORMAT_R8G8B8A8_UINT"}, {TexelFormat::R8G8B8A8Snorm, 77, "PIPE_FORMAT_R8G8B8A8_SNORM"},
    {TexelFormat::R8G8B8A8Sint, 184, "PIPE_FORMAT_R8G8B8A8_SINT"}, {TexelFormat::R16G16Float, 92, "PIPE_FORMAT_R16G16_FLOAT"},
    {TexelFormat::R16G16Unorm, 49, "PIPE_FORMAT_R16G16_UNORM"}, {TexelFormat::R16G16Uint, 186, "PIPE_FORMAT_R16G16_UINT"},
    {TexelFormat::R16G16Snorm, 57, "PIPE_FORMAT_R16G16_SNORM"}, {TexelFormat::R16G16Sint, 190, "PIPE_FORMAT_R16G16_SINT"},
    {TexelFormat::R32Float, 28, "PIPE_FORMAT_R32_FLOAT"}, {TexelFormat::R32Uint, 193, "PIPE_FORMAT_R32_UINT"}, {TexelFormat::R32Sint, 197, "PIPE_FORMAT_R32_SINT"},
    {TexelFormat::R8G8Unorm, 65, "PIPE_FORMAT_R8G8_UNORM"}, {TexelFormat::R8G8Uint, 178, "PIPE_FORMAT_R8G8_UINT"}, {TexelFormat::R8G8Snorm, 75, "PIPE_FORMAT_R8G8_SNORM"},
    {TexelFormat::R8G8Sint, 182, "PIPE_FORMAT_R8G8_SINT"}, {TexelFormat::R16Float, 91, "PIPE_FORMAT_R16_FLOAT"}, {TexelFormat::R16Unorm, 48, "PIPE_FORMAT_R16_UNORM"},
    {TexelFormat::R16Uint, 185, "PIPE_FORMAT_R16_UINT"}, {TexelFormat::R16Snorm, 56, "PIPE_FORMAT_R16_SNORM"}, {TexelFormat::R16Sint, 189, "PIPE_FORMAT_R16_SINT"},
    {TexelFormat::R8Unorm, 64, "PIPE_FORMAT_R8_UNORM"}, {TexelFormat::R8Uint, 177, "PIPE_FORMAT_R8_UINT"}, {TexelFormat::R8Snorm, 74, "PIPE_FORMAT_R8_SNORM"},
    {TexelFormat::R8Sint, 181, "PIPE_FORMAT_R8_SINT"}, {TexelFormat::A8Unorm, 10, "PIPE_FORMAT_A8_UNORM"}, {TexelFormat::B5G6R5Unorm, 7, "PIPE_FORMAT_B5G6R5_UNORM"},
    {TexelFormat::B5G5R5A1Unorm, 5, "PIPE_FORMAT_B5G5R5A1_UNORM"}, {TexelFormat::B8G8R8A8Unorm, 1, "PIPE_FORMAT_B8G8R8A8_UNORM"},
    {TexelFormat::B8G8R8X8Unorm, 2, "PIPE_FORMAT_B8G8R8X8_UNORM"}, {TexelFormat::B4G4R4A4Unorm, 6, "PIPE_FORMAT_B4G4R4A4_UNORM"},
};
uint32_t texel_virgl_format(TexelFormat f) {
  for (const TexelHostFormat& h : kTexelHostFormats)
    if (h.format == f) return h.virgl;
  return 0;
}
// Whether the host's buffer textures take the format (OpenGL's glTexBuffer formats, ARB_texture_buffer_object and _rgb32: one to four channels of 8, 16 and 32-bit unsigned normalised,
// float and integer values, three of 32 bits): both the views of t# (sampler views of the buffer) and those of u# (images of it, which the host makes of a buffer texture) need one.
// Not the snorm ones, R10G10B10A2, R11G11B10, A8 or the B-first packed ones.
bool texel_host_buffer_format(TexelFormat f) {
  const TexelLayout l = texel_layout(f);
  return l.kind == TexelKind::Float || l.kind == TexelKind::Uint || l.kind == TexelKind::Sint || (l.kind == TexelKind::Unorm && f != TexelFormat::A8Unorm);
}
const char* texel_format_name(uint32_t virgl_format) {
  for (const TexelHostFormat& h : kTexelHostFormats)
    if (h.virgl == virgl_format) return h.name;
  return nullptr;
}

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
  VirglImage(VirglDevice& device, const ImageDesc& desc) : device_(device), desc_(desc), surfaces_(device.gpu().allocator()) {
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
        if (static_cast<uint32_t>(desc.format) >= static_cast<uint32_t>(Format::R8Unorm)) {
          color_format_ = virgl::extra_format(desc.format);   // (the formats numbered from 64: the host's format of the same layout)
          break;
        }
        color_format_ = (desc.usage & kImageStorage) ? virgl::kFormatR8G8B8A8Unorm : virgl::kFormatB8G8R8A8Unorm;
        swap_rb_ = (desc.usage & kImageStorage) != 0;
        break;
    }
    const uint32_t depth_format = desc.stencil ? virgl::kFormatZ32FloatS8X24Uint : virgl::kFormatZ32Float;
    // A sampled image is stored top row first, the way write_pixels() gives it and the sampler reads it, so it does not take the Y_0_TOP flip of the images that are only
    // drawn into (and shown).
    const bool sampled = (desc.usage & kImageSampled) != 0;
    // (a format the host cannot draw into - block-compressed, and the formats numbered from 64 its render mask does not have - is a sampler view alone)
    const bool renderable = !is_compressed_format(desc.format) && (static_cast<uint32_t>(desc.format) < static_cast<uint32_t>(Format::R8Unorm) || gpu.virgl_format_renderable(color_format_));
    uint32_t bind = depth ? virgl::kBindDepthStencil | (sampled ? virgl::kBindSamplerView : 0u) : renderable ? (virgl::kBindRenderTarget | virgl::kBindSamplerView) : virgl::kBindSamplerView;
    if (desc.usage & kImagePresent) bind |= virgl::kBindScanout;
    // A volume has its slices as the depth; an array, a cube or a cube array its layers as the array size (a cube: six). A cube that is a storage image is an array on the host -
    // the image units take a 2D array of its faces, which a texture of the cube target is not - and is sampled as a cube through a view of that target.
    const uint32_t layers = layer_count();
    const bool cube = desc.kind == TextureKind::TextureCube || desc.kind == TextureKind::TextureCubeArray;
    const uint32_t target = desc.kind == TextureKind::Texture3D ? virgl::kTargetTexture3d
                            : cube && (desc.usage & kImageStorage) ? virgl::kTargetTexture2dArray
                            : desc.kind == TextureKind::TextureCube ? virgl::kTargetTextureCube
                            : desc.kind == TextureKind::TextureCubeArray ? virgl::kTargetTextureCubeArray
                            : desc.kind == TextureKind::Texture2DArray ? virgl::kTargetTexture2dArray : virgl::kTargetTexture2d;
    resource_target_ = target;
    ResourceCreate3d create{header(kCmdResourceCreate3d), id_, target,
                            depth ? depth_format : color_format_, bind,
                            desc.extent.width, desc.extent.height, desc.kind == TextureKind::Texture3D ? layers : 1u, desc.kind == TextureKind::Texture3D || desc.kind == TextureKind::Texture2D ? 1u : layers, desc.mip_levels - 1 /* last level */, desc.samples > 1 ? desc.samples : 0u,
                            sampled || desc.kind != TextureKind::Texture2D ? 0u : kResourceFlagY0Top, 0};   // (the host takes the flag on 2D images alone: a layered depth buffer is kept the GL way)
    CtxResource ctx{header(kCmdCtxAttachResource), id_, 0};
    ok_ = gpu.command(as_bytes(create)) && gpu.command(as_bytes(ctx));
    accounted_ = 0;   // (every level of every layer)
    for (uint32_t level = 0; level < desc.mip_levels; ++level)
      accounted_ += uint64_t{mip_extent(desc.extent.width, level)} * mip_extent(desc.extent.height, level) * level_layers(level) * desc.samples * (depth ? 4u : bytes_per_pixel(desc.format));
    gpu.add_resource_bytes(accounted_);
    // The surface (a render-target view of the resource) is what the framebuffer state refers to.
    if (ok_ && (desc.usage & (kImageColorAttachment | kImageDepthAttachment))) {
      surface_ = surface(0, 0, 1);
      ok_ = surface_ != 0;
    }
    // The sampler view is what a texture unit is bound to: the whole resource, all of its levels, components in order.
    if (ok_ && sampled) {
      view_ = device.new_handle();
      // (the layers the view shows: all of an array's or a cube's; a volume's slices are not layers. A storage cube's view is of the cube target, its resource an array)
      const uint32_t last_layer = desc.kind == TextureKind::Texture2DArray || cube ? layers - 1 : 0;
      const uint32_t view_target = cube && (desc.usage & kImageStorage) ? (desc.kind == TextureKind::TextureCube ? virgl::kTargetTextureCube : virgl::kTargetTextureCubeArray) : 0;
      // (a depth buffer read as a texture: its depth in red, 0, 0, 1 - Direct3D's R32_FLOAT / R24_UNORM_X8 views -; one with a stencil plane has a view of that too, the stencil
      // in green as an integer - X24_TYPELESS_G8_UINT -, which the host reads with GL_STENCIL_INDEX texturing)
      const uint32_t cmd[] = {virgl::header(virgl::kCreateObject, virgl::kObjectSamplerView, 6), view_, id_, (depth ? depth_format : color_format_) | view_target << 24, last_layer << 16,
                              (desc.mip_levels - 1) << 8 /* first level | last level << 8 */, depth ? virgl::kSwizzleDepth : virgl::kSwizzleRgba};
      ok_ = device.send_commands(cmd, 7);
      if (ok_ && depth && desc.stencil) {
        stencil_view_ = device.new_handle();
        const uint32_t stencil_cmd[] = {virgl::header(virgl::kCreateObject, virgl::kObjectSamplerView, 6), stencil_view_, id_, virgl::kFormatX32S8X24Uint, last_layer << 16,
                                        (desc.mip_levels - 1) << 8, virgl::kSwizzleStencil};
        ok_ = device.send_commands(stencil_cmd, 7);
      }
    }
  }
  ~VirglImage() {
    hardware::VirtioGpu& gpu = device_.gpu();
    if (view_) device_.destroy_object(view_);
    if (stencil_view_) device_.destroy_object(stencil_view_);
    if (stand_in_) destroy(gpu.allocator(), stand_in_);
    if (fetch_view_) device_.destroy_object(fetch_view_);
    for (const LayerSurface& s : surfaces_.view())
      if (s.handle) device_.destroy_object(s.handle);
    for (const View& v : views_)
      if (v.handle) device_.destroy_object(v.handle);
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
  // The host's surface of layers first .. first + count - 1 of level `level` (a layered one draws into the layer gl_Layer names, from the first), made on first use (the oldest of a full
  // cache is let go of first; never one the pass being recorded uses); 0 if the host refuses it.
  uint32_t surface(uint32_t level, uint32_t first, uint32_t count) {
    for (const LayerSurface& s : surfaces_.view())
      if (s.handle && s.level == level && s.first == first && s.count == count) return s.handle;
    if (count == 0 || level >= desc_.mip_levels || first >= level_layers(level) || count > level_layers(level) - first) return 0;
    // (kept as long as the image: a submit may name any number of them - every level of every face of a cube array drawn into or cleared one at a time -, and a surface the
    // commands being encoded name may not go before they are sent)
    const uint32_t handle = device_.new_handle();
    const uint32_t fmt = desc_.format == Format::D32Float ? (desc_.stencil ? virgl::kFormatZ32FloatS8X24Uint : virgl::kFormatZ32Float) : color_format_;
    const uint32_t cmd[] = {virgl::header(virgl::kCreateObject, virgl::kObjectSurface, 5), handle, id_, fmt, level, first | ((first + count - 1) << 16)};
    if (!device_.send_commands(cmd, 6)) return 0;
    if (!surfaces_.push_back(LayerSurface{level, first, count, handle})) {
      device_.destroy_object(handle);
      return 0;
    }
    return handle;
  }
  // The layers of level `level` (a volume's slices there).
  uint32_t level_layers(uint32_t level) const { return level_layer_count(desc_.kind, desc_.layers, level); }
  TextureKind kind() const { return desc_.kind; }
  // The host's sampler view of a TextureView (kFeature2MipmappedLayers: texture_view_fits, the counts made whole), made on first use: levels base .. base + count - 1, layers (a
  // volume: all its slices, as the host counts them none) first .. first + count - 1, of the view's target - a texture view of the resource on the host when it is another than
  // the resource's, or a part of it; of a depth buffer with a stencil plane, of that plane with `stencil` (SamplerDesc::stencil). 0 if it cannot be made.
  uint32_t view(const TextureView& requested, bool stencil = false) {
    stencil = stencil && stencil_view_;
    TextureView v = requested;
    if (v.level_count == 0 && v.base_level < desc_.mip_levels) v.level_count = desc_.mip_levels - v.base_level;
    if (v.layer_count == 0 && v.first_layer < layer_count()) v.layer_count = layer_count() - v.first_layer;
    if (!view_ || !texture_view_fits(desc_.kind, layer_count(), desc_.mip_levels, v)) return 0;
    if (v.kind == desc_.kind && v.base_level == 0 && v.level_count == desc_.mip_levels && v.first_layer == 0 && v.layer_count == layer_count()) return view(stencil);   // (the whole of it)
    for (const View& c : views_)
      if (c.handle && c.stencil == stencil && c.v.kind == v.kind && c.v.base_level == v.base_level && c.v.level_count == v.level_count && c.v.first_layer == v.first_layer &&
          c.v.layer_count == v.layer_count)
        return c.handle;
    View* free = nullptr;
    for (View& c : views_)
      if (!c.handle) { free = &c; break; }
    if (!free) {   // (a full cache: the first goes; the commands recorded so far that name it ran when the view was made again - the host keeps an object a binding uses)
      free = &views_[next_evicted_view_++ % kViews];
      device_.destroy_object(free->handle);
      free->handle = 0;
    }
    // (a multisampled one's target is the resource's - 2D or 2D array -, the samples go with the resource)
    const uint32_t target = v.kind == TextureKind::Texture3D ? virgl::kTargetTexture3d : v.kind == TextureKind::TextureCube ? virgl::kTargetTextureCube
                            : v.kind == TextureKind::TextureCubeArray ? virgl::kTargetTextureCubeArray : v.kind == TextureKind::Texture2DArray || v.kind == TextureKind::Texture2DMSArray ? virgl::kTargetTexture2dArray
                                                                                                          : virgl::kTargetTexture2d;
    const uint32_t first_layer = v.kind == TextureKind::Texture3D ? 0 : v.first_layer, last_layer = v.kind == TextureKind::Texture3D ? 0 : v.first_layer + v.layer_count - 1;
    const uint32_t handle = device_.new_handle();
    // (a depth buffer's view: of its depth, or of its stencil plane - as the image's own views are)
    const bool depth = desc_.format == Format::D32Float;
    const uint32_t format = !depth ? color_format_ : stencil ? virgl::kFormatX32S8X24Uint : desc_.stencil ? virgl::kFormatZ32FloatS8X24Uint : virgl::kFormatZ32Float;
    const uint32_t swizzle = !depth ? virgl::kSwizzleRgba : stencil ? virgl::kSwizzleStencil : virgl::kSwizzleDepth;
    const uint32_t cmd[] = {virgl::header(virgl::kCreateObject, virgl::kObjectSamplerView, 6), handle, id_, format | target << 24, first_layer | last_layer << 16,
                            v.base_level | (v.base_level + v.level_count - 1) << 8, swizzle};
    if (!device_.send_commands(cmd, 7)) return 0;
    *free = View{v, stencil, handle};
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
      default: return static_cast<uint32_t>(f) >= static_cast<uint32_t>(Format::R8Unorm) ? virgl::extra_format(f) : virgl::kFormatR8G8B8A8Unorm;
    }
  }
  uint32_t view() const { return view_; }
  // The view a unit reads: the stencil plane's of a depth buffer that has one (SamplerDesc::stencil), else the image's.
  uint32_t view(bool stencil) const { return stencil && stencil_view_ ? stencil_view_ : view_; }
  // A depth buffer read as a texture is kept the way textures are (row 0 first, no Y_0_TOP): a pass that draws into it with a colour target kept the other way (an image that is
  // only drawn into and shown) would leave its rows the other way round too. Such a pass draws into this upright stand-in for the colour target instead, which is filled from the
  // target first (unless the pass clears it) and copied into it after the pass; the host's blits turn the rows over between the two kinds of image. Made on first use, kept
  // with the image; null if the host will not make it.
  VirglImage* stand_in() {
    if (stand_in_) return stand_in_;
    ImageDesc d = desc_;
    d.usage = kImageColorAttachment | kImageSampled;
    VirglImage* made = make<VirglImage>(device_.gpu().allocator(), device_, d);
    if (made && !made->valid()) {
      destroy(device_.gpu().allocator(), made);
      made = nullptr;
    }
    stand_in_ = made;
    return stand_in_;
  }
  // A depth buffer read as a texture (kFeature2DepthTextures).
  bool sampled_depth() const { return desc_.format == Format::D32Float && (desc_.usage & kImageSampled) != 0; }
  // A view for texel fetches: the sampled image's own, or one made on first use for an image that is not sampled (a multisampled render target, which the least /
  // greatest resolve reads sample by sample). 0 for a depth image or if the host refuses it.
  uint32_t fetch_view() {
    if (view_) return view_;
    if (fetch_view_ || desc_.format == Format::D32Float || is_compressed_format(desc_.format)) return fetch_view_;
    const uint32_t handle = device_.new_handle();
    const uint32_t cmd[] = {virgl::header(virgl::kCreateObject, virgl::kObjectSamplerView, 6), handle, id_, color_format_, 0, 0, virgl::kSwizzleRgba};
    if (device_.send_commands(cmd, 7)) fetch_view_ = handle;
    return fetch_view_;
  }
  Extent2D extent() const override { return desc_.extent; }
  Format format() const override { return desc_.format; }
  uint32_t usage() const { return desc_.usage; }
  uint32_t samples() const { return desc_.samples; }
  bool has_stencil() const { return desc_.stencil; }
  uint32_t blit_format() const { return desc_.format == Format::D32Float ? (desc_.stencil ? virgl::kFormatZ32FloatS8X24Uint : virgl::kFormatZ32Float) : color_format_; }   // (what a blit calls the image's format)
  uint32_t color_format() const { return color_format_; }
  bool stencil() const { return desc_.stencil; }
  uint32_t layer_count() const override { return image_layer_count(desc_.kind, desc_.layers); }
  uint32_t mip_levels() const override { return desc_.mip_levels; }
  // Layer `layer` of level `level` of any image of one sample (a volume's slice there), staged at the start of the backing (which has room for level 0 of a layer, the largest): the
  // transfer's box is the level's size at depth `layer`, its stride a row of the level. Level 0 of a 2D image goes the way write_pixels takes it (an image only drawn into and shown
  // is kept top row first).
  bool write_subresource(uint32_t level, uint32_t layer, ArrayView<const uint32_t> pixels) override {
    if (level == 0 && layer == 0 && layer_count() == 1) return write_pixels(pixels);
    const uint32_t width = mip_extent(desc_.extent.width, level), height = mip_extent(desc_.extent.height, level);
    const uint64_t count = uint64_t{width} * height;
    if (desc_.samples != 1 || desc_.format == Format::D32Float || is_wide_format(desc_.format) || is_compressed_format(desc_.format) || level >= desc_.mip_levels ||
        layer >= level_layers(level) || pixels.size() != count || !ensure_backing(uint64_t{desc_.extent.width} * desc_.extent.height))
      return false;
    const uint32_t texel = stage(pixels);   // (0xAARRGGBB words, or the 1 or 2 bytes of a packed 32-bit format)
    if (!texel) return false;
    TransferHost3d transfer{header(kCmdTransferToHost3d), {0, 0, layer, width, height, 1}, 0, id_, level, width * texel, 0};
    return device_.gpu().command(as_bytes(transfer), /*fence=*/false);
  }
  bool read_subresource(uint32_t level, uint32_t layer, ArrayView<uint32_t> out) override {
    if (level == 0 && layer == 0 && layer_count() == 1) return read_pixels(out);
    const uint32_t width = mip_extent(desc_.extent.width, level), height = mip_extent(desc_.extent.height, level);
    const uint64_t count = uint64_t{width} * height;
    if (desc_.samples != 1 || desc_.format == Format::D32Float || is_wide_format(desc_.format) || is_compressed_format(desc_.format) || level >= desc_.mip_levels ||
        layer >= level_layers(level) || out.size() < count || !ensure_backing(uint64_t{desc_.extent.width} * desc_.extent.height))
      return false;
    TransferHost3d transfer{header(kCmdTransferFromHost3d), {0, 0, layer, width, height, 1}, 0, id_, level, width * staged_texel_bytes(), 0};
    if (!device_.gpu().command(as_bytes(transfer))) return false;
    return unstage(out.first(count));
  }
  bool write_subresource_float(uint32_t level, uint32_t layer, ArrayView<const Vec4> pixels) override {
    if (level == 0 && layer == 0 && layer_count() == 1) return write_pixels_float(pixels);
    const uint32_t width = mip_extent(desc_.extent.width, level), height = mip_extent(desc_.extent.height, level);
    const uint64_t count = uint64_t{width} * height;
    if (!is_wide_format(desc_.format) || desc_.samples != 1 || level >= desc_.mip_levels || layer >= level_layers(level) || pixels.size() != count ||
        !ensure_backing(uint64_t{desc_.extent.width} * desc_.extent.height))
      return false;
    ByteWriter out{backing_};
    for (const Vec4& c : pixels) encode_wide_pixel(desc_.format, c, out);
    if (!out.ok()) return false;
    TransferHost3d transfer{header(kCmdTransferToHost3d), {0, 0, layer, width, height, 1}, 0, id_, level, width * bytes_per_pixel(desc_.format), 0};
    return device_.gpu().command(as_bytes(transfer), /*fence=*/false);
  }
  bool read_subresource_float(uint32_t level, uint32_t layer, ArrayView<Vec4> out) override {
    if (level == 0 && layer == 0 && layer_count() == 1) return read_pixels_float(out);
    const uint32_t width = mip_extent(desc_.extent.width, level), height = mip_extent(desc_.extent.height, level);
    const uint64_t count = uint64_t{width} * height;
    if (!is_wide_format(desc_.format) || desc_.samples != 1 || level >= desc_.mip_levels || layer >= level_layers(level) || out.size() < count ||
        !ensure_backing(uint64_t{desc_.extent.width} * desc_.extent.height))
      return false;
    TransferHost3d transfer{header(kCmdTransferFromHost3d), {0, 0, layer, width, height, 1}, 0, id_, level, width * bytes_per_pixel(desc_.format), 0};
    if (!device_.gpu().command(as_bytes(transfer))) return false;
    ByteReader in{ConstByteView(backing_)};
    for (uint64_t i = 0; i < count; ++i) out[i] = decode_wide_pixel(desc_.format, in);
    return in.ok();
  }
  // The planes of a depth image of one sample (kFeature2DepthCopies), through the backing (room for level 0 of a layer: 4 bytes a texel, or with a stencil plane 8 - the host's
  // Z32_FLOAT_S8X24_UINT transfers a float, then a word with the stencil in its low byte). The host turns the rows of an image kept the other way round, as for colour images.
  bool depth_transfer(uint32_t level, uint32_t layer, bool to_host) {
    const uint32_t width = mip_extent(desc_.extent.width, level), height = mip_extent(desc_.extent.height, level);
    TransferHost3d transfer{header(to_host ? kCmdTransferToHost3d : kCmdTransferFromHost3d), {0, 0, layer, width, height, 1}, 0, id_, level, width * (desc_.stencil ? 8u : 4u), 0};
    return device_.gpu().command(as_bytes(transfer), /*fence=*/!to_host);
  }
  bool depth_planes_ok(uint32_t level, uint32_t layer, uint64_t depth, uint64_t stencil) {
    const uint64_t count = uint64_t{mip_extent(desc_.extent.width, level)} * mip_extent(desc_.extent.height, level);
    return desc_.format == Format::D32Float && desc_.samples == 1 && level < desc_.mip_levels && layer < level_layers(level) && (depth == 0 || depth == count) &&
           (stencil == 0 || (stencil == count && desc_.stencil)) && ensure_backing(uint64_t{desc_.extent.width} * desc_.extent.height * (desc_.stencil ? 2u : 1u));
  }
  bool read_depth_subresource(uint32_t level, uint32_t layer, ArrayView<float> depth, ArrayView<uint8_t> stencil) override {
    if (!depth_planes_ok(level, layer, depth.size(), stencil.size()) || !depth_transfer(level, layer, false)) return false;
    ByteReader in{ConstByteView(backing_)};
    const uint64_t count = depth.empty() ? stencil.size() : depth.size();
    for (uint64_t i = 0; i < count; ++i) {
      const float d = __builtin_bit_cast(float, in.le32());
      const uint32_t s = desc_.stencil ? in.le32() : 0u;
      if (!depth.empty()) depth[i] = d;
      if (!stencil.empty()) stencil[i] = static_cast<uint8_t>(s & 0xFF);
    }
    return in.ok();
  }
  bool write_depth_subresource(uint32_t level, uint32_t layer, ArrayView<const float> depth, ArrayView<const uint8_t> stencil) override {
    if (!depth_planes_ok(level, layer, depth.size(), stencil.size()) || (depth.empty() && stencil.empty())) return false;
    // (one plane of two: the other as the host has it, read first)
    if (desc_.stencil && (depth.empty() || stencil.empty()) && !depth_transfer(level, layer, false)) return false;
    const uint64_t count = depth.empty() ? stencil.size() : depth.size();
    const uint32_t texel = desc_.stencil ? 8u : 4u;
    for (uint64_t i = 0; i < count; ++i) {
      ByteView at;
      if (!backing_.try_subview(i * texel, texel, &at)) return false;
      if (!depth.empty()) {
        const float v = depth[i];
        ByteWriter out{at.subview(0, 4)};
        out.le32(__builtin_bit_cast(uint32_t, !(v == v) ? 0.0f : v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v));   // (a depth image keeps 0 .. 1)
        if (!out.ok()) return false;
      }
      if (!stencil.empty()) {
        ByteWriter out{at.subview(4, 4)};
        out.le32(stencil[i]);
        if (!out.ok()) return false;
      }
    }
    return depth_transfer(level, layer, true);
  }
  // A whole subresource of another image (kFeature2DepthCopies: depth and multisampled images, which have no memory here): a blit of it, of the planes asked for, turning the rows over
  // between images kept in the two orders (multisampled images are both kept the way GL keeps them).
  bool copy_subresource(IImage& source_image, uint32_t level, uint32_t layer, uint32_t source_level, uint32_t source_layer, uint32_t planes) override {
    VirglImage& from = static_cast<VirglImage&>(source_image);   // (the kernel copies between images of one device)
    const bool depth = desc_.format == Format::D32Float;
    const uint32_t width = mip_extent(desc_.extent.width, level), height = mip_extent(desc_.extent.height, level);
    if (from.desc_.format != desc_.format || from.desc_.samples != desc_.samples || from.desc_.stencil != desc_.stencil || level >= desc_.mip_levels || layer >= level_layers(level) ||
        source_level >= from.desc_.mip_levels || source_layer >= from.level_layers(source_level) || mip_extent(from.desc_.extent.width, source_level) != width ||
        mip_extent(from.desc_.extent.height, source_level) != height || planes == 0 || planes > 3 || (!depth && planes != 1) || ((planes & 2) && !desc_.stencil))
      return false;
    const uint32_t mask = depth ? ((planes & 1) ? 0x10u : 0u) | ((planes & 2) ? 0x20u : 0u) : 0xFu;
    const bool flip = desc_.samples == 1 && y0_top() != from.y0_top();
    const uint32_t cmd[22] = {virgl::header(virgl::kBlit, 0, 21), mask, 0, width | (height << 16), id_, level, blit_format(), 0, flip ? height : 0u, layer, width,
                              flip ? static_cast<uint32_t>(-static_cast<int32_t>(height)) : height, 1, from.id_, source_level, from.blit_format(), 0, 0, source_layer, width, height, 1};
    return device_.send_commands(cmd, 22);
  }
  // A level above 0 of a mipmapped texture: staged at the start of the backing (which has room for level 0, the largest), from where the host copies it into the level.
  bool write_mip_pixels(uint32_t level, ArrayView<const uint32_t> pixels) override {
    if (level == 0) return write_pixels(pixels);
    const uint32_t width = mip_extent(desc_.extent.width, level), height = mip_extent(desc_.extent.height, level);
    const uint64_t count = uint64_t{width} * height;
    if (level >= desc_.mip_levels || pixels.size() != count || !ensure_backing(uint64_t{desc_.extent.width} * desc_.extent.height)) return false;
    const uint32_t texel = stage(pixels);
    if (!texel) return false;
    TransferHost3d transfer{header(kCmdTransferToHost3d), {0, 0, 0, width, height, 1}, 0, id_, level, width * texel, 0};
    return device_.gpu().command(as_bytes(transfer), /*fence=*/false);
  }
  // The blocks of a level of a layer (a slice, a face; 0 for a plain texture) of a block-compressed image: staged at the start of the backing (room for level 0 of a layer, as
  // 32-bit pixels, which is more than its blocks), the transfer's stride a row of blocks, its box in pixels.
  bool write_compressed(uint32_t level, uint32_t layer, ConstByteView blocks) override {
    if (!is_compressed_format(desc_.format) || level >= desc_.mip_levels || layer >= level_layers(level)) return false;
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
    const uint32_t texel = stage(pixels);
    if (!texel) return false;
    TransferHost3d transfer{header(kCmdTransferToHost3d), {0, 0, layer, desc_.extent.width, desc_.extent.height, 1}, 0, id_, 0, desc_.extent.width * texel, 0};
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
    TransferHost3d transfer{header(kCmdTransferFromHost3d), {0, 0, layer, desc_.extent.width, desc_.extent.height, 1}, 0, id_, 0, desc_.extent.width * staged_texel_bytes(), 0};
    if (!device_.gpu().command(as_bytes(transfer))) return false;
    return unstage(out.first(count));
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
    TransferHost3d transfer{header(kCmdTransferFromHost3d), {0, 0, 0, width, height, 1}, 0, id_, level, width * staged_texel_bytes(), 0};
    if (!device_.gpu().command(as_bytes(transfer))) return false;
    return unstage(out.first(count));
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
                            desc_.extent.width * staged_texel_bytes(), 0};
    if (!gpu.command(as_bytes(transfer))) return false;
    return unstage(out.first(count));
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
    const uint32_t texel = stage(pixels);
    if (!texel) return false;
    TransferHost3d transfer{header(kCmdTransferToHost3d), {0, 0, 0, desc_.extent.width, desc_.extent.height, 1}, 0, id_, 0, desc_.extent.width * texel, 0};
    return device_.gpu().command(as_bytes(transfer), /*fence=*/false);
  }

  // A rectangle: its rows go to the same place in the backing (the backing is the whole image), and the host copies just that box.
  bool write_rect(const Rect2D& area, ArrayView<const uint32_t> pixels, uint32_t pitch) override {
    const Extent2D e = desc_.extent;
    if (desc_.samples > 1 || desc_.format == Format::D32Float || is_wide_format(desc_.format) || is_packed32_format(desc_.format) || !area.width || !area.height || pitch < area.width) return false;
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

  // The bytes of a texel in the backing: 4 (0xAARRGGBB words), or the 1 or 2 of a packed 32-bit format, which the host keeps in its own layout.
  uint32_t staged_texel_bytes() const { return is_packed32_format(desc_.format) ? bytes_per_pixel(desc_.format) : 4u; }
  // 0xAARRGGBB words into the start of the backing as the host takes them; the texel bytes (0: they do not fit).
  uint32_t stage(ArrayView<const uint32_t> pixels) {
    if (!is_packed32_format(desc_.format)) {
      ArrayView<uint32_t> staged;
      if (!backing_.try_cast<uint32_t>(&staged) || staged.size() < pixels.size()) return 0;
      for (uint64_t i = 0; i < pixels.size(); ++i) staged[i] = pixels[i];
      swap_words(staged.first(pixels.size()));
      return 4;
    }
    const uint32_t texel = bytes_per_pixel(desc_.format);
    ByteWriter out{backing_};
    for (const uint32_t p : pixels) {
      const uint32_t v = packed32_from_argb(desc_.format, p);
      if (texel == 1) out.u8(static_cast<uint8_t>(v));
      else out.le16(static_cast<uint16_t>(v));
    }
    return out.ok() ? texel : 0;
  }
  // The start of the backing (what the host copied into it) as 0xAARRGGBB words.
  bool unstage(ArrayView<uint32_t> out) const {
    if (!is_packed32_format(desc_.format)) {
      ArrayView<uint32_t> pixels;
      if (!backing_.try_cast<uint32_t>(&pixels) || pixels.size() < out.size()) return false;
      for (uint64_t i = 0; i < out.size(); ++i) out[i] = pixels[i];
      swap_words(out);
      return true;
    }
    const uint32_t texel = bytes_per_pixel(desc_.format);
    ByteReader in{ConstByteView(backing_)};
    for (uint32_t& p : out) p = packed32_to_argb(desc_.format, texel == 1 ? in.u8() : in.le16());
    return in.ok();
  }
  // (a storage image of 8-bit colour is RGBA on the host: the 0xAARRGGBB words it is written and read in have red and blue turned around)
  uint32_t swap_word(uint32_t w) const { return swap_rb_ ? (w & 0xFF00FF00u) | ((w & 0xFF) << 16) | ((w >> 16) & 0xFF) : w; }
  void swap_words(ArrayView<uint32_t> words) const {
    if (!swap_rb_) return;
    for (uint32_t& w : words) w = swap_word(w);
  }
  struct LayerSurface {
    uint32_t level = 0, first = 0, count = 0, handle = 0;
  };
  struct View {
    TextureView v;
    bool stencil = false;   // (of a depth buffer's stencil plane)
    uint32_t handle = 0;
  };
  static constexpr uint32_t kViews = 16;   // (the views shader resource views name: a few per image)
  View views_[kViews];
  uint32_t next_evicted_view_ = 0;
  uint32_t resource_target_ = 0;
  bool swap_rb_ = false;
  VirglDevice& device_;
  ImageDesc desc_;
  List<LayerSurface> surfaces_;   // (the levels and layer ranges render target and depth views name)
  uint32_t id_ = 0, surface_ = 0, view_ = 0, color_format_ = 0;
  uint32_t stencil_view_ = 0;          // a depth buffer read as a texture: the view of its stencil plane
  VirglImage* stand_in_ = nullptr;     // (stand_in)
  uint32_t fetch_view_ = 0;   // (fetch_view)
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
    sampler_constants_ = desc.sampler_constants;
    desc_ = desc;
    // A shader with storage images or typed views of buffers names their formats and views by marks (IMGFMT<k> and the others of TgsiBindings): the host's shader is made for what a
    // dispatch or draw binds (variant), not now. A copy with a stream output declaration (a pipeline's) cannot be: the variants are made without one.
    if (desc.tgsi && has_marks(desc.tgsi)) {
      has_images_ = true;
      ok_ = stream_output == nullptr;
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
  bool sampler_constants() const { return sampler_constants_; }   // (ShaderModuleDesc::sampler_constants)
  const TgsiBindings& bindings() const { return desc_.tgsi_bindings; }
  // What a variant of the shader is made for: the format of what is bound to each IMAGE[k] (VIRGL_FORMAT_*), the first element and the count of the views of buffers there, and those of
  // the buffer views SVIEW[kTgsiTexelViewBase + k] (the elements the host's view starts before the view's first, and the view's count).
  struct VariantKey {
    uint32_t formats[kTgsiMaxImages] = {};
    uint32_t image_first[kTgsiMaxImages] = {}, image_count[kTgsiMaxImages] = {};
    uint32_t texel_first[kTgsiMaxTexelViews] = {}, texel_count[kTgsiMaxTexelViews] = {};
  };
  // The host's shader for `key`: each mark of the text made what the key says (IMGFMT<k> the name of IMAGE[k]'s format, IMGOFS<k> / IMGLEN<k> / TBOFS<k> / TBLEN<k> its numbers); made on
  // first use and kept with the module (kVariants of them; the oldest goes when there is no room). 0 when the host refuses it.
  uint32_t variant(VirglDevice& device, const VariantKey& key) const {
    for (const Variant& v : variants_)
      if (v.handle && __builtin_memcmp(&v.key, &key, sizeof key) == 0) return v.handle;
    const uint32_t length = string_length(tgsi_);
    // (each mark becomes a format's name, at most 32 characters, or a number of at most 10)
    uint32_t marks = 0;
    for (const char* p = tgsi_; *p; ++p)
      if (mark_at(p, nullptr, nullptr)) ++marks;
    const uint64_t capacity = uint64_t{length} + uint64_t{marks} * 32 + 1;
    char* text = static_cast<char*>(device.gpu().allocator().allocate(capacity));
    if (!text) return 0;
    uint64_t n = 0;
    for (const char* p = tgsi_; *p;) {
      uint32_t kind = 0, index = 0;
      if (const uint32_t taken = mark_at(p, &kind, &index)) {
        char digits[12];
        const char* replacement = digits;
        if (kind == 0) {
          replacement = format_name(index < kTgsiMaxImages ? key.formats[index] : 0);
        } else {
          const uint32_t value = kind == 1 ? (index < kTgsiMaxImages ? key.image_first[index] : 0) : kind == 2 ? (index < kTgsiMaxImages ? key.image_count[index] : 0)
                                 : kind == 3 ? (index < kTgsiMaxTexelViews ? key.texel_first[index] : 0) : (index < kTgsiMaxTexelViews ? key.texel_count[index] : 0);
          uint32_t v = value, at = sizeof digits - 1;
          digits[at] = 0;
          do {
            digits[--at] = static_cast<char>('0' + v % 10);
            v /= 10;
          } while (v && at > 0);
          replacement = digits + at;
        }
        for (const char* c = replacement; *c && n + 1 < capacity; ++c) text[n++] = *c;
        p += taken;
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
    slot.key = key;
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
  // A mark of TgsiBindings at `p` (IMGFMT, IMGOFS, IMGLEN, TBOFS or TBLEN and a number): its length, with what it is (0 .. 4 in that order) and the number; 0 for none.
  static uint32_t mark_at(const char* p, uint32_t* kind, uint32_t* index) {
    static const char* const kNames[] = {"IMGFMT", "IMGOFS", "IMGLEN", "TBOFS", "TBLEN"};
    for (uint32_t k = 0; k < 5; ++k) {
      uint32_t i = 0;
      while (kNames[k][i] && p[i] == kNames[k][i]) ++i;
      if (kNames[k][i] || p[i] < '0' || p[i] > '9') continue;
      uint32_t number = 0, length = i;
      while (p[length] >= '0' && p[length] <= '9' && length < i + 4) number = number * 10 + static_cast<uint32_t>(p[length++] - '0');
      if (kind) *kind = k;
      if (index) *index = number;
      return length;
    }
    return 0;
  }
  static bool has_marks(const char* text) {
    for (const char* p = text; *p; ++p)
      if (mark_at(p, nullptr, nullptr)) return true;
    return false;
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
      default: {
        const char* name = texel_format_name(format);
        return name ? name : "PIPE_FORMAT_R8G8B8A8_UNORM";
      }
    }
  }
  struct Variant {
    VariantKey key;
    uint32_t handle = 0;
  };
  static constexpr uint32_t kVariants = 8;
  mutable Variant variants_[kVariants];
  mutable uint32_t next_variant_ = 0;
  bool has_images_ = false;
  bool sampler_constants_ = false;
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
  if (format == VertexFormat::Bgra8Unorm || format == VertexFormat::Bgrx8Unorm) return virgl::kFormatB8G8R8A8Unorm;   // (B8G8R8X8: its w made 1 in the shader, VirglVertexPatch.hpp)
  if (format == VertexFormat::Rgb10A2Unorm) return virgl::kFormatR10G10B10A2Unorm;
  if (format == VertexFormat::Rgb10A2Uint) return virgl::kFormatR32Uint;   // (the word, unpacked in the shader: OpenGL has no such integer attribute)
  if (format == VertexFormat::Rg11B10Float) return 124;   // R11G11B10_FLOAT (GL_UNSIGNED_INT_10F_11F_11F_REV)
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
    // Vertex formats the host does not read as Direct3D does (VirglVertexPatch.hpp): the vertex shader is a copy of the module's text that makes them right. It has the stream output
    // declaration when the vertex shader is the last stage before the rasterizer.
    {
      virgl_patch::InputFix fixes[kMaxVertexAttributes] = {};
      bool any = false;
      for (uint32_t i = 0; i < desc.attribute_count; ++i) {
        const VertexAttribute& a = desc.attributes[i];
        if (a.location >= kMaxVertexAttributes) return;
        if (a.format == VertexFormat::Rgb10A2Uint) fixes[a.location] = virgl_patch::InputFix::UnpackRgb10A2Uint, any = true;
        else if (a.format == VertexFormat::Bgrx8Unorm) fixes[a.location] = virgl_patch::InputFix::ForceW1, any = true;
      }
      if (any) {
        const auto* vertex = static_cast<const VirglShaderModule*>(desc.vertex_shader);
        if (!vertex->tgsi()) return;
        String text(device.gpu().allocator());
        if (!virgl_patch::patch_vertex_inputs(StringView(vertex->tgsi()), ArrayView<const virgl_patch::InputFix>(fixes, kMaxVertexAttributes), &text)) return;
        ShaderModuleDesc variant;
        variant.stage = ShaderStage::Vertex;
        variant.tgsi = text.c_str();
        VirglShaderModule module(device, variant, desc.stream_output_count && !gs_ && !ds_ ? &desc : nullptr);
        if (!module.valid()) return;
        vs_ = own_vs_ = module.handle();
      }
    }
    // Stream output goes with the last vertex-processing shader on the host (the geometry shader, else the domain shader, else the vertex shader): a copy of it that has the declaration.
    if (desc.stream_output_count) {
      if (!desc.geometry_shader && !desc.domain_shader && !own_vs_) {
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
    if (desc.stream_output_count && desc.geometry_shader) {
      const auto* geometry = static_cast<const VirglShaderModule*>(desc.geometry_shader);
      if (!geometry->tgsi() || geometry->has_images()) return;   // (a copy with the declaration cannot be made per binding)
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
    if (desc.hull_shader && desc.domain_shader) {
      const auto* hull = static_cast<const VirglShaderModule*>(desc.hull_shader);
      const auto* domain = static_cast<const VirglShaderModule*>(desc.domain_shader);
      // (a domain shader with storage images or views of buffers is made per binding - variants of its own text, not of a copy: so it goes only with a hull shader of its own winding and
      // partitioning, without stream output)
      if ((hull->tess_output() != domain->tess_output() || hull->tess_partitioning() != domain->tess_partitioning() || (desc.stream_output_count && !desc.geometry_shader)) && domain->has_images()) return;
      if ((hull->tess_output() != domain->tess_output() || hull->tess_partitioning() != domain->tess_partitioning() || (desc.stream_output_count && !desc.geometry_shader)) && domain->tgsi()) {
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
        VirglShaderModule module(device, variant, desc.stream_output_count && !desc.geometry_shader ? &desc : nullptr);
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
    stencil_back_reference_ = st.enable ? (desc.separate_back_stencil ? desc.back_stencil.reference : st.reference) : 0;
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
    // (kFeature2LineModes: alpha-antialiased lines are the host's smooth lines - GL_LINE_SMOOTH, whose coverage the host multiplies into the alpha -, quadrilateral lines its
    // multisampled lines, which GL draws as rectangles of the line width on a multisampled target - on a target of one sample the host draws its own aliased line)
    const bool quad_lines = desc.line_mode == LineMode::QuadrilateralWide || desc.line_mode == LineMode::QuadrilateralNarrow;
    const bool offset = desc.depth_bias.enabled() && !desc.depth_bias.unorm_bits;   // (a normalised format's bias is the fragment shader's)
    cmd[n++] = (desc.depth_clip ? 1u << 1 : 0u) /*depth clip*/ | (1u << 2) /*clip half z*/ | (desc.stream_output_count && desc.rasterized_stream < 0 ? 1u << 3 : 0u) /*rasterizer discard*/ | (cull << 8) |
               (fill << 10) | (fill << 12) /*fill front, back*/ | (1u << 14) /*scissor*/ | ((front_ccw ? 1u : 0u) << 15) | (offset ? (1u << 18) | (1u << 20) : 0u) /*offset line (a wireframe triangle's), tri*/ |
               (desc.multisample || quad_lines ? 1u << 25 : 0u) /*multisample*/ | (desc.line_mode == LineMode::AlphaAntialiased ? 1u << 26 : 0u) /*line smooth*/ |
               (1u << 29) /*half pixel center*/ | (1u << 30) /*bottom edge rule*/;
    cmd[n++] = float_bits(1.0f);  // point size
    cmd[n++] = 0;                 // sprite coord enable
    cmd[n++] = uint32_t{desc.clip_distances} << 24;   // line stipple, clip planes (the clip distances enabled)
    cmd[n++] = float_bits(desc.line_mode == LineMode::QuadrilateralWide ? 1.4f : 1.0f);  // line width (Direct3D's quadrilateral lines: 1.4 or 1.0 pixels)
    cmd[n++] = float_bits(offset ? static_cast<float>(desc.depth_bias.constant) : 0.0f);   // offset units
    cmd[n++] = float_bits(offset ? desc.depth_bias.slope : 0.0f);                                                         // offset scale
    cmd[n++] = float_bits(offset ? desc.depth_bias.clamp : 0.0f);                                                         // offset clamp

    // Vertex elements. Element n is the vertex shader's input n on the host, so they are laid out by location (a location nothing feeds repeats the first attribute: the
    // shader does not read it). The host keeps the instance divisor with a buffer binding, and sets binding n's divisor from element n (glVertexBindingDivisor(n, ...) for
    // each element n): so element n reads binding m, the first element with its slot and divisor, whose divisor element m set. Binding n is slot n's element's slot
    // (bindings_[], which the draw fills with the slots' buffers), used or not. A step rate of 0 (every instance reads the draw's first instance) is the largest divisor:
    // OpenGL's divisor 0 is per vertex.
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
    auto divisor_of = [](const VertexAttribute& a) { return !a.per_instance ? 0u : a.step_rate ? a.step_rate : 0xFFFFFFFFu; };
    auto attribute_at = [&](uint32_t location) -> const VertexAttribute& { return desc.attributes[by_location[location] != kMaxVertexAttributes ? by_location[location] : 0]; };
    for (uint32_t location = 0; location < element_count; ++location) {
      const VertexAttribute& a = attribute_at(location);
      uint32_t binding = 0;
      while (binding < location && (attribute_at(binding).slot != a.slot || divisor_of(attribute_at(binding)) != divisor_of(a))) ++binding;
      bindings_[location] = a.slot;
      cmd[n++] = a.offset;
      cmd[n++] = divisor_of(a);   // instance divisor
      cmd[n++] = binding;
      cmd[n++] = virgl_vertex_format(a.format);
    }
    binding_count_ = element_count;
    ok_ = n <= sizeof cmd / sizeof cmd[0] && device.send_commands(cmd, n);
  }
  ~VirglPipeline() {
    if (own_fs_) device_.destroy_object(own_fs_);
    if (own_vs_) device_.destroy_object(own_vs_);
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
  uint32_t binding_slot(uint32_t binding) const { return bindings_[binding]; }
  uint8_t stencil_reference() const { return stencil_reference_; }
  uint8_t stencil_back_reference() const { return stencil_back_reference_; }   // (the second DSA stencil state's)
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
  // Whether a shader of the pipeline reads the bound samplers' comparisons from the constants (ShaderModuleDesc::sampler_constants).
  bool sampler_constants() const {
    for (const IShaderModule* m : {desc_.vertex_shader, desc_.fragment_shader, desc_.geometry_shader, desc_.hull_shader, desc_.domain_shader})
      if (m && static_cast<const VirglShaderModule*>(m)->sampler_constants()) return true;
    return false;
  }
  // A fragment shader with storage images: the host's shader is the module's variant for the images bound at the draw.
  const VirglShaderModule* fragment_module() const { return static_cast<const VirglShaderModule*>(desc_.fragment_shader); }
  // The module of a stage (virgl::kShader*: vertex, fragment, geometry, tessellation control and evaluation), null for one the pipeline does not have.
  const VirglShaderModule* stage_module(uint32_t stage) const {
    const IShaderModule* m = stage == virgl::kShaderVertex ? desc_.vertex_shader : stage == virgl::kShaderFragment ? desc_.fragment_shader : stage == virgl::kShaderGeometry ? desc_.geometry_shader
                             : stage == virgl::kShaderTessControl ? desc_.hull_shader : stage == virgl::kShaderTessEval ? desc_.domain_shader : nullptr;
    return static_cast<const VirglShaderModule*>(m);
  }
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
  uint8_t stencil_reference_ = 0, stencil_back_reference_ = 0;
  uint32_t blend_ = 0, dsa_ = 0, rasterizer_ = 0, elements_ = 0;
  uint32_t own_fs_ = 0;   // (a fragment shader of the pipeline's own: the depth bias of a normalised format)
  uint8_t bindings_[kMaxVertexAttributes] = {};   // the slot of each host vertex buffer binding
  uint32_t own_vs_ = 0;   // the vertex shader made for this pipeline's vertex formats (0: none)
  uint32_t binding_count_ = 0;
  bool ok_ = false;
};

// The least or greatest of each pixel's samples of a multisampled colour image `from` into the plain image `to` of its size: a triangle over the whole of `to` whose pixel
// shader fetches every sample of its pixel (TXF on the 2D_MSAA view, the sample in the fourth component) and keeps the least or the greatest of each channel. The shaders and
// states are made for the resolve and destroyed after it in the stream (the host runs the stream in order); the framebuffer, viewport, shaders, blend, depth, rasterizer
// and vertex elements it binds are the caller's to set again.
bool extreme_resolve(VirglDevice& device, Stream& stream, VirglImage& to, VirglImage& from, bool greatest) {
  const uint32_t samples = from.samples();
  const uint32_t view = from.fetch_view(), surface = to.surface(0, 0, 1);
  if (samples < 2 || samples > 16 || !view || !surface) return false;
  // (the vertex shader: a triangle from (-1, -1) to (3, -1) and (-1, 3), from the vertex number alone - x is 4 * (id & 1) - 1, y 4 * ((id & 2) != 0) - 1)
  static const char kVertex[] =
      "VERT\n"
      "DCL SV[0], VERTEXID\n"
      "DCL OUT[0], POSITION\n"
      "DCL TEMP[0]\n"
      "IMM[0] UINT32 {1, 2, 0, 1065353216}\n"
      "IMM[1] FLT32 {4.0000, -1.0000, 0.0000, 1.0000}\n"
      "  0: AND TEMP[0].xy, SV[0].xxxx, IMM[0].xyyy\n"
      "  1: USNE TEMP[0].xy, TEMP[0].xyyy, IMM[0].zzzz\n"
      "  2: AND TEMP[0].xy, TEMP[0].xyyy, IMM[0].wwww\n"
      "  3: MAD OUT[0].xy, TEMP[0].xyyy, IMM[1].xxxx, IMM[1].yyyy\n"
      "  4: MOV OUT[0].zw, IMM[1].zzzw\n"
      "  5: END\n";
  // (the pixel shader: the pixel's coordinate as integers, then each sample fetched and folded in with MIN or MAX)
  char fragment[2048];
  uint32_t n = 0;
  auto add = [&](const char* text) {
    for (const char* c = text; *c && n + 1 < sizeof fragment; ++c) fragment[n++] = *c;
    fragment[n] = 0;
  };
  auto add_number = [&](uint32_t v) {
    char digits[12];
    uint32_t d = 0;
    do { digits[d++] = static_cast<char>('0' + v % 10); v /= 10; } while (v && d < sizeof digits);
    while (d && n + 1 < sizeof fragment) fragment[n++] = digits[--d];
    fragment[n] = 0;
  };
  add("FRAG\nPROPERTY FS_COLOR0_WRITES_ALL_CBUFS 1\nDCL IN[0], POSITION, LINEAR\nDCL OUT[0], COLOR\nDCL SAMP[0]\nDCL SVIEW[0], 2D_MSAA, FLOAT\nDCL TEMP[0..2]\n");
  add("IMM[0] UINT32 {0, 0, 0, 0}\n");
  for (uint32_t k = 1; k < samples; ++k) {
    add("IMM[");
    add_number(k);
    add("] UINT32 {");
    add_number(k);
    add(", 0, 0, 0}\n");
  }
  uint32_t line = 0;
  auto instruction = [&](const char* text) {
    add("  ");
    add_number(line++);
    add(": ");
    add(text);
    add("\n");
  };
  instruction("F2I TEMP[0].xy, IN[0].xyyy");
  instruction("MOV TEMP[0].zw, IMM[0].xxxx");
  instruction("TXF TEMP[1], TEMP[0], SAMP[0], 2D_MSAA");
  for (uint32_t k = 1; k < samples; ++k) {
    add("  ");
    add_number(line++);
    add(": MOV TEMP[0].w, IMM[");
    add_number(k);
    add("].xxxx\n");
    instruction("TXF TEMP[2], TEMP[0], SAMP[0], 2D_MSAA");
    instruction(greatest ? "MAX TEMP[1], TEMP[1], TEMP[2]" : "MIN TEMP[1], TEMP[1], TEMP[2]");
  }
  instruction("MOV OUT[0], TEMP[1]");
  instruction("END");
  if (n + 1 >= sizeof fragment) return false;
  // (the objects, made straight away so that they exist before the stream that uses them)
  const uint32_t vs = device.new_handle(), fs = device.new_handle(), blend = device.new_handle(), dsa = device.new_handle(), rasterizer = device.new_handle(), elements = device.new_handle();
  auto shader = [&](uint32_t handle, uint32_t stage, const char* text) {
    const uint32_t length = string_length(text) + 1;
    const uint32_t dwords = (length + 3) / 4;
    uint32_t cmd[6 + 2048 / 4 + 1] = {};
    if (6 + dwords > sizeof cmd / sizeof cmd[0]) return false;
    cmd[0] = virgl::header(virgl::kCreateObject, virgl::kObjectShader, 5 + dwords);
    cmd[1] = handle;
    cmd[2] = stage;
    cmd[3] = length;
    cmd[4] = 300;   // (the TGSI tokens the host reserves)
    cmd[5] = 0;     // (no stream output)
    __builtin_memcpy(&cmd[6], text, length);
    return device.send_commands(cmd, 6 + dwords);
  };
  const uint32_t states[] = {
      virgl::header(virgl::kCreateObject, virgl::kObjectBlend, 11), blend, 0, 0, 0xFu << 27, 0, 0, 0, 0, 0, 0, 0,
      virgl::header(virgl::kCreateObject, virgl::kObjectDsa, 5), dsa, 0, 0, 0, 0,
      // (depth clip, a 0..1 clip volume, no culling, no scissor, GL pixel conventions)
      virgl::header(virgl::kCreateObject, virgl::kObjectRasterizer, 9), rasterizer, (1u << 1) | (1u << 2) | (virgl::kFaceNone << 8) | (1u << 29) | (1u << 30), float_bits(1.0f), 0, 0,
      float_bits(1.0f), 0, 0, 0,
      // (one element the vertex shader does not read)
      virgl::header(virgl::kCreateObject, virgl::kObjectVertexElements, 5), elements, 0, 0, 0, virgl::kFormatR32G32B32A32Float,
  };
  if (!shader(vs, virgl::kShaderVertex, kVertex) || !shader(fs, virgl::kShaderFragment, fragment) || !device.send_commands(states, sizeof states / sizeof states[0])) return false;
  const uint32_t sampler = device.sampler_state(SamplerDesc());
  if (!sampler) return false;
  const Extent2D e = to.extent();
  stream.command(virgl::kSetFramebufferState, 0, 3);
  stream.put(1);
  stream.put(0);
  stream.put(surface);
  stream.command(virgl::kSetViewportState, 0, 7);
  stream.put(0);
  stream.putf(e.width * 0.5f);
  stream.putf((to.y0_top() ? -1.0f : 1.0f) * e.height * 0.5f);
  stream.putf(1.0f);
  stream.putf(e.width * 0.5f);
  stream.putf(e.height * 0.5f);
  stream.putf(0.0f);
  stream.command(virgl::kBindObject, virgl::kObjectBlend, 1);
  stream.put(blend);
  stream.command(virgl::kBindObject, virgl::kObjectDsa, 1);
  stream.put(dsa);
  stream.command(virgl::kBindObject, virgl::kObjectRasterizer, 1);
  stream.put(rasterizer);
  stream.command(virgl::kBindObject, virgl::kObjectVertexElements, 1);
  stream.put(elements);
  const uint32_t binds[][2] = {{vs, virgl::kShaderVertex}, {fs, virgl::kShaderFragment}, {0, virgl::kShaderGeometry}, {0, virgl::kShaderTessControl}, {0, virgl::kShaderTessEval}};
  for (const auto& b : binds) {
    stream.command(virgl::kBindShader, 0, 2);
    stream.put(b[0]);
    stream.put(b[1]);
  }
  stream.command(virgl::kSetSamplerViews, 0, 3);
  stream.put(virgl::kShaderFragment);
  stream.put(0);
  stream.put(view);
  stream.command(virgl::kBindSamplerStates, 0, 3);
  stream.put(virgl::kShaderFragment);
  stream.put(0);
  stream.put(sampler);
  stream.command(virgl::kDrawVbo, 0, 12);
  stream.put(0);   // (first vertex)
  stream.put(3);
  stream.put(virgl::kPrimTriangles);
  stream.put(0);   // (not indexed)
  stream.put(1);   // (instances)
  stream.put(0);
  stream.put(0);
  stream.put(0);
  stream.put(0);
  stream.put(0);
  stream.put(0xFFFFFFFFu);
  stream.put(0);
  for (const uint32_t handle : {vs, fs, blend, dsa, rasterizer, elements}) {
    stream.command(virgl::kDestroyObject, 0, 1);
    stream.put(handle);
  }
  return !stream.overflow();
}

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
// The sampler a texture unit is bound with: an integer texture's is nearest - the host's GL takes an integer texture with a linear filter as incomplete and reads zeros, also in
// texel fetches, where Direct3D's Load ignores the sampler -, any other's as it is.
namespace {
SamplerDesc unfiltered_for(SamplerDesc sampler, const VirglImage* texture) {
  if (texture && is_integer_format(texture->format())) {
    sampler.filter = sampler.mag_filter = Filter::Nearest;
    if (sampler.mip_filter == MipFilter::Linear) sampler.mip_filter = MipFilter::Nearest;
  }
  return sampler;
}
}  // namespace

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
  const Filter mag = magnification_filter(sampler);   // (kFeature2SeparateFilters: the host's own magnification filter)
  wanted.bits = (sampler.filter == Filter::Linear ? 1u : 0u) | (u << 1) | (v << 4) | ((bordered && !sampler.custom_border ? border : 0u) << 7) | (mip << 9) | (anisotropy << 16) |
                ((mag == Filter::Linear ? 1u : 0u) << 24);
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
  const uint32_t filter = sampler.filter == Filter::Linear ? virgl::kFilterLinear : virgl::kFilterNearest, mag_filter = mag == Filter::Linear ? virgl::kFilterLinear : virgl::kFilterNearest;
  // (S0: the wrap modes, filters, seamless cube maps - Direct3D always filters across the edges of a cube -, and the anisotropy in bits 20 - 24; the border colour's bits as they
  // are: the host sets it with glSamplerParameterIuiv, so an integer texture's border is its integers and a float one's its floats)
  const uint32_t s0 = kWrap[u] | (kWrap[v] << 3) | (virgl::kWrapClampToEdge << 6) /* r: the slices of a volume are clamped */ | (filter << 9) | (kMip[mip] << 11) | (mag_filter << 13) | (1u << 19) |
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

uint32_t VirglDevice::format_caps(Format f) const {
  const uint32_t host = virgl::extra_format(f);
  if (!host || !gpu_.virgl_format_sampled(host)) return 0;
  if (is_compressed_format(f)) return kFormatSampled | kFormatFiltered | kFormatMipmaps | kFormatNative;
  const bool integer = is_integer_format(f);
  uint32_t caps = kFormatSampled | (integer ? 0u : kFormatFiltered);
  if (gpu_.virgl_format_renderable(host) && f != Format::R9G9B9E5Float) caps |= kFormatRenderTarget | (integer ? 0u : kFormatBlendable);
  // (levels: of the packed 32-bit formats, staged as the host keeps them; of every format where the device has levels of every image - the host's texture views, kFeature2MipmappedLayers)
  if (is_packed32_format(f) || (info().features2 & kFeature2MipmappedLayers)) caps |= kFormatMipmaps;
  const bool image_unit = f != Format::A8Unorm && f != Format::B5G6R5Unorm && f != Format::B5G5R5A1Unorm && f != Format::B4G4R4A4Unorm && f != Format::R9G9B9E5Float &&
                          f != Format::R32G32B32Uint && f != Format::R32G32B32Sint && f != Format::R32G32B32Float;
  if (image_unit && (info().features2 & kFeature2StorageImages)) caps |= kFormatStorage;
  return caps;
}
uint32_t VirglDevice::max_image_dimension() const {
  const uint32_t host = gpu_.virgl_max_texture_2d();
  return host < 4096 ? 4096 : host > 16384 ? 16384 : host;
}

IImage* VirglDevice::create_image(const ImageDesc& desc) {
  if (desc.extent.width > max_image_dimension() || desc.extent.height > max_image_dimension()) return nullptr;
  // (a format numbered from 64: what format_caps says of it)
  const bool extra = static_cast<uint32_t>(desc.format) >= static_cast<uint32_t>(Format::R8Unorm);
  if (extra) {
    const uint32_t caps = format_caps(desc.format);
    if (!(caps & kFormatSampled) || ((desc.usage & kImageColorAttachment) && !(caps & kFormatRenderTarget)) || ((desc.usage & kImageStorage) && !(caps & kFormatStorage)) ||
        (desc.mip_levels > 1 && !(caps & kFormatMipmaps)) || desc.samples != 1)
      return nullptr;
  }
  // (mipmaps: a plain texture of 8-bit colour that is only sampled, as the kernel's own; with kFeature2MipmappedLayers any image of one sample - arrays, volumes, cubes, cube arrays,
  // targets, depth buffers, storage images, the wide formats -; its levels are written one by one)
  if (desc.mip_levels == 0 || desc.mip_levels > mip_level_limit(desc.extent)) return nullptr;
  const bool all_levels = (info().features2 & kFeature2MipmappedLayers) != 0;
  const uint32_t layers = image_layer_count(desc.kind, desc.layers);
  const bool cube = desc.kind == TextureKind::TextureCube || desc.kind == TextureKind::TextureCubeArray;
  if (desc.kind == TextureKind::Texture2DMS || (desc.kind == TextureKind::TextureCubeArray && (!all_levels || layers % 6 != 0))) return nullptr;
  if (is_compressed_format(desc.format)) {
    // (block-compressed: sampled only, of any kind, plain textures with levels - and the others too with kFeature2MipmappedLayers; filled with write_compressed)
    if (desc.usage != kImageSampled || desc.samples != 1 || layers == 0 || layers > kMaxTextureLayers || (desc.kind != TextureKind::Texture2D && desc.mip_levels != 1 && !all_levels) ||
        (desc.kind == TextureKind::Texture2D && desc.layers != 1) || (cube && desc.extent.width != desc.extent.height))
      return nullptr;
    VirglImage* image = make<VirglImage>(gpu_.allocator(), *this, desc);
    if (image && !image->valid()) {
      destroy(gpu_.allocator(), image);
      return nullptr;
    }
    return image;
  }
  // (a storage image: sampled too, one sample, not a cube - but with kFeature2MipmappedLayers a cube too, kept as an array of its faces; the host's image units take it as it is kept,
  // row 0 first)
  if ((desc.usage & kImageStorage) && (!(desc.usage & kImageSampled) || desc.samples != 1 || (cube && !all_levels) || desc.format == Format::D32Float || is_srgb_format(desc.format) ||
                                       !(info().features2 & kFeature2StorageImages)))
    return nullptr;
  if (desc.mip_levels != 1 && !all_levels && ((desc.usage & ~kImageStorage) != kImageSampled || desc.kind != TextureKind::Texture2D || desc.samples != 1 || desc.layers != 1 ||
                                               (desc.format != Format::B8G8R8A8Unorm && desc.format != Format::B8G8R8A8Srgb && desc.format != Format::R8G8B8A8Unorm && desc.format != Format::R8G8B8A8Srgb && !is_packed32_format(desc.format))))
    return nullptr;
  if (desc.mip_levels != 1 && (desc.samples != 1 || (desc.usage & kImagePresent))) return nullptr;
  const bool multisampled = desc.samples != 1;
  if (multisampled) {
    // (a multisampled image is an 8-bit colour image or a depth attachment, which a shader may read with texel fetches (kFeature3MultisampleLoads / MultisampleDepthLoads), resolved; a
    // 2D image, or an array of them - kFeature3MultisampleArrays - drawn into a layer at a time)
    const bool depth = desc.format == Format::D32Float;
    if ((desc.samples != 2 && desc.samples != 4) || (desc.kind != TextureKind::Texture2D && desc.kind != TextureKind::Texture2DArray) ||
        (depth ? (desc.usage & ~kImageSampled) != kImageDepthAttachment : (desc.usage & ~kImageSampled) != kImageColorAttachment) || (!depth && desc.format != Format::B8G8R8A8Unorm && desc.format != Format::R8G8B8A8Unorm) ||
        desc.layers == 0 || desc.layers > kMaxTextureLayers || (desc.kind == TextureKind::Texture2D && desc.layers != 1) ||
        (desc.kind == TextureKind::Texture2DArray && !(info().features3 & kFeature3MultisampleArrays)))
      return nullptr;
  }
  if (multisampled) {
    // (checked above)
  } else if (desc.kind != TextureKind::Texture2D) {
    // (a volume, array, cube or cube array: sampled - and drawn into, or a storage image, with kFeature2LayeredTargets / StorageImages -, 8-bit colour or a wide format (its layers
    // staged as such: write_subresource_float), one level or with kFeature2MipmappedLayers several; or a depth buffer of layers, an array, a cube or a cube array - maybe sampled too)
    const bool depth_layers = desc.format == Format::D32Float && (desc.usage & ~kImageSampled) == kImageDepthAttachment && desc.kind != TextureKind::Texture3D;
    const bool colour = (desc.usage & kImageSampled) && !(desc.usage & ~(kImageSampled | kImageColorAttachment | kImageStorage)) &&
                        (desc.format == Format::B8G8R8A8Unorm || desc.format == Format::R8G8B8A8Unorm || is_wide_format(desc.format) || is_packed32_format(desc.format));
    if ((!colour && !depth_layers) || layers == 0 || layers > kMaxTextureLayers || (desc.mip_levels != 1 && !all_levels) || (cube && desc.extent.width != desc.extent.height)) return nullptr;
    if ((depth_layers || (desc.usage & kImageColorAttachment)) && !(info().features2 & kFeature2LayeredTargets)) return nullptr;
  } else if (desc.layers != 1) {
    return nullptr;
  }
  // A texture is not shown: the two kinds of image are stored in opposite row order (see VirglImage). A depth buffer that is read as a texture is kept the way textures are, and
  // a pass that draws into it with a colour target kept the other way draws into a stand-in for the target (VirglImage::stand_in). (Such a depth buffer has one level, or any with
  // kFeature2MipmappedLayers: its sampler views show the levels as a colour texture's do.)
  if ((desc.usage & kImageSampled) && (desc.usage & kImagePresent)) return nullptr;
  if ((desc.usage & kImageSampled) && (desc.usage & kImageDepthAttachment) &&
      (desc.format != Format::D32Float || (desc.mip_levels != 1 && !all_levels) || (desc.usage & ~(kImageSampled | kImageDepthAttachment))))
    return nullptr;
  if ((desc.usage & kImageSampled) && desc.format != Format::D32Float && desc.format != Format::B8G8R8A8Unorm && desc.format != Format::B8G8R8A8Srgb && desc.format != Format::R8G8B8A8Unorm && desc.format != Format::R8G8B8A8Srgb && !is_wide_format(desc.format) && !is_packed32_format(desc.format)) return nullptr;
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

  // Queries: the host's query objects, each with a buffer of its own for its result (16 bytes: state, size, the count) that the host fills in when asked for it after the commands
  // have run (the host takes no offset into a shared buffer: two queries on one buffer overwrite each other). Ours may be any number open at once, of one kind or several and nested,
  // while the host runs one of each target at a time: a target's host queries are segments, one ending where the next begins whenever one of ours that needs the target begins or ends
  // (and around a blit, which the host may draw), and each of ours adds up the segments of each of its targets between its begin and its end. The slots from kMaxOcclusionQueries on
  // are the device's own: the stream output primitives a transform feedback wrote, to know how far a buffer got.
  struct Segment {
    uint32_t target;
    uint32_t host;   // (the index in Queries::host)
  };
  struct UserQuery {
    bool open = false, synthetic = false;
    QueryKind kind = QueryKind::Occlusion;
    uint32_t stream = 0, targets = 0;
    uint64_t synthetic_value = 0;
    uint32_t timestamp = 0;   // (the host query of a timestamp)
    uint64_t first[kQueryTargets] = {}, end[kQueryTargets] = {};   // (the segments it spans: indices in Queries::segments, from first to before end)
  };
  struct HostQuery {
    VirglBuffer* result = nullptr;
    uint32_t object = 0;
  };
  struct Queries {
    explicit Queries(VirglDevice& d) : device(d), host(d.gpu().allocator()), segments(d.gpu().allocator()), users(d.gpu().allocator()) {}
    ~Queries() {
      for (const HostQuery& q : host.view()) {
        if (q.object) device.destroy_object(q.object);
        if (q.result) destroy(device.gpu().allocator(), q.result);
      }
    }
    VirglDevice& device;
    List<HostQuery> host;
    List<Segment> segments;
    List<UserQuery> users;
    int32_t slot_user[kQuerySlots];            // (the index in `users` of a slot, -1 for none)
    int64_t active[kQueryTargets];             // (the segment of a target the host is running, -1 for none)
    uint32_t open_users[kQueryTargets] = {};   // (how many of ours that need the target are open)
    uint32_t open_count = 0;
  } queries(*this);
  for (int32_t& u : queries.slot_user) u = -1;
  for (int64_t& a : queries.active) a = -1;
  bool disjoint_asked = false;
  // A transform feedback is alive on the host (from the first draw with stream output to its end): only then do the targets of the primitives written and needed run. (A pipeline
  // without stream output writes and needs none; and the host draws wrongly with a PRIMITIVES_GENERATED query running and no transform feedback.)
  bool so_set = false, so_alive = false;
  auto target_runs = [&](uint32_t target) { return target < kTargetWritten || so_alive; };
  // A host query of `type` made and begun (and ended at once, for a timestamp): its index in queries.host, or -1.
  auto host_query = [&](uint32_t type, bool end_now) -> int64_t {
    HostQuery q;
    q.result = make<VirglBuffer>(gpu_.allocator(), *this, 16, 0, virgl::kBindCustom);
    if (!q.result) return -1;
    if (!q.result->valid() || !queries.host.push_back(q)) {
      destroy(gpu_.allocator(), q.result);
      return -1;
    }
    HostQuery& made = queries.host[queries.host.count() - 1];
    made.object = new_handle();
    stream.command(virgl::kCreateObject, virgl::kObjectQuery, 4);
    stream.put(made.object);
    stream.put(type);
    stream.put(0);
    stream.put(made.result->id());
    stream.command(virgl::kBeginQuery, 0, 1);
    stream.put(made.object);
    if (end_now) {
      stream.command(virgl::kEndQuery, 0, 1);
      stream.put(made.object);
    }
    return static_cast<int64_t>(queries.host.count() - 1);
  };
  auto begin_segment = [&](uint32_t target) {
    if (!target_runs(target)) return true;   // (counted from when it runs: while a transform feedback is alive)
    const int64_t h = host_query(host_query_type(target), false);
    if (h < 0 || !queries.segments.push_back({target, static_cast<uint32_t>(h)})) return false;
    queries.active[target] = static_cast<int64_t>(queries.segments.count() - 1);
    return true;
  };
  auto end_segment = [&](uint32_t target) {
    if (queries.active[target] < 0) return;
    stream.command(virgl::kEndQuery, 0, 1);
    stream.put(queries.host[queries.segments[static_cast<uint64_t>(queries.active[target])].host].object);
    queries.active[target] = -1;
  };
  // Begins query `slot` of ours (false if it cannot be: a slot begun twice, no memory).
  auto begin_user = [&](uint32_t slot, QueryKind kind, uint32_t output_stream) {
    if (slot >= kQuerySlots || queries.slot_user[slot] >= 0) return false;
    UserQuery u;
    u.open = true;
    u.kind = kind;
    u.stream = output_stream;
    u.targets = query_targets(kind, output_stream);
    if (kind == QueryKind::TimestampFrequency || kind == QueryKind::TimestampDisjoint) {
      // (the host has no such query: its timestamps are nanoseconds and it cannot say whether the clock was interrupted)
      u.synthetic = true;
      u.synthetic_value = kind == QueryKind::TimestampFrequency ? 1000000000u : (clock_interrupted_ ? 1u : 0u);
      disjoint_asked = disjoint_asked || kind == QueryKind::TimestampDisjoint;
    }
    for (uint32_t t = 0; t < kQueryTargets; ++t) {
      if (!(u.targets >> t & 1)) continue;
      end_segment(t);   // (the others that need it go on in the next segment)
      if (!begin_segment(t)) return false;
      u.first[t] = queries.active[t] >= 0 ? static_cast<uint64_t>(queries.active[t]) : queries.segments.count();
      ++queries.open_users[t];
    }
    if (!queries.users.push_back(u)) return false;
    queries.slot_user[slot] = static_cast<int32_t>(queries.users.count() - 1);
    ++queries.open_count;
    return true;
  };
  auto end_user = [&](uint32_t slot) {
    if (slot >= kQuerySlots || queries.slot_user[slot] < 0) return false;
    UserQuery& u = queries.users[static_cast<uint64_t>(queries.slot_user[slot])];
    if (!u.open) return false;
    u.open = false;
    --queries.open_count;
    if (u.kind == QueryKind::Timestamp) {
      const int64_t h = host_query(virgl::kQueryTimestamp, true);
      if (h < 0) return false;
      u.timestamp = static_cast<uint32_t>(h);
    }
    for (uint32_t t = 0; t < kQueryTargets; ++t) {
      if (!(u.targets >> t & 1)) continue;
      u.end[t] = queries.active[t] >= 0 ? static_cast<uint64_t>(queries.active[t]) + 1 : queries.segments.count();
      end_segment(t);
      if (--queries.open_users[t] && !begin_segment(t)) return false;
    }
    return true;
  };
  // The query of the commands begun last that is still open (what an EndQuery that names none ends): its slot, or kQuerySlots.
  auto last_open_slot = [&]() {
    uint32_t slot = kQuerySlots;
    int32_t latest = -1;
    for (uint32_t k = 0; k < kMaxOcclusionQueries; ++k) {
      const int32_t u = queries.slot_user[k];
      if (u > latest && queries.users[static_cast<uint64_t>(u)].open) latest = u, slot = k;
    }
    return slot;
  };
  // Around a command that is not a draw or a dispatch (a blit, which the host may make with draws of its own): no segment runs.
  auto pause_queries = [&]() {
    for (uint32_t t = 0; t < kQueryTargets; ++t) end_segment(t);
  };
  auto resume_queries = [&]() {
    for (uint32_t t = 0; t < kQueryTargets; ++t)
      if (queries.open_users[t] && queries.active[t] < 0 && !begin_segment(t)) return false;
    return true;
  };

  const VirglPipeline* pipeline = nullptr;
  VirglImage* color = nullptr;
  VirglImage* extra_colors[kMaxColorTargets - 1] = {};
  uint32_t target_count = 1;
  VirglImage* depth = nullptr;
  VirglBuffer* vertex_buffers[kMaxVertexBuffers] = {};
  uint64_t vertex_offsets[kMaxVertexBuffers] = {};
  const VirglImage* textures[kMaxTextureUnits] = {};
  SamplerDesc samplers[kMaxTextureUnits];
  // The host's sampler view each unit is bound to (the image's own, or one of a view: bind_texture_view), and the levels and layers it shows (whole: all of them).
  uint32_t texture_views[kMaxTextureUnits] = {};
  TextureView texture_ranges[kMaxTextureUnits];
  // The open pass's targets and the level and layers of each it draws into (kMaxColorTargets: the depth buffer): a unit may sample other levels or layers of them.
  const VirglImage* pass_target[kMaxColorTargets + 1] = {};
  uint32_t pass_level[kMaxColorTargets + 1] = {}, pass_first[kMaxColorTargets + 1] = {}, pass_count[kMaxColorTargets + 1] = {};
  auto drawn_into = [&](uint32_t unit) {
    for (uint32_t n = 0; n <= kMaxColorTargets; ++n) {
      if (!pass_target[n] || pass_target[n] != textures[unit]) continue;
      const TextureView& v = texture_ranges[unit];
      if (pass_level[n] < v.base_level || pass_level[n] >= v.base_level + v.level_count) continue;
      if (textures[unit]->kind() == TextureKind::Texture3D || (pass_first[n] < v.first_layer + v.layer_count && v.first_layer < pass_first[n] + pass_count[n])) return true;
    }
    return false;
  };
  // The storage images bound (bind_storage_image), the typed views of buffers (bind_storage_texel_buffer: of the u# slots and of the t# ones), and the host's shaders bound for each
  // stage (virgl::kShader*: the variants of modules that have images).
  VirglImage* images[kMaxStorageImages] = {};
  StorageImageView image_views[kMaxStorageImages];
  struct TexelBinding {
    VirglBuffer* buffer = nullptr;
    TexelBufferView view;
    uint32_t host_view = 0;   // (a t# one: the host's buffer view of it, made when a draw or dispatch first needs it)
  };
  TexelBinding texels[kMaxImageSlots];
  uint32_t bound_shader[6] = {};
  // The host's buffer views made in this submit: destroyed when it ends (after its commands went out), or when there are too many (all of them then, after what was recorded).
  struct MadeViews {
    VirglDevice& device;
    uint32_t handles[256] = {};
    uint32_t count = 0;
    void destroy_all() {
      for (uint32_t k = 0; k < count; ++k) device.destroy_object(handles[k]);
      count = 0;
    }
    ~MadeViews() { destroy_all(); }
  } made_views{*this};
  bool draw_bindings_dirty = false, compute_bindings_dirty = false;
  bool textures_dirty = false, compute_textures_dirty = false, compute_textures_changed = false;
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
  // The constants a stage is given: the push constants and, for a pipeline whose shaders compare (ShaderModuleDesc::sampler_constants), the comparisons of the units' samplers
  // after the most there may be (kSamplerCompareRegister) - sent again when the samplers bound change.
  bool sampler_constants_dirty = false;
  uint32_t pass_samples = 1;   // (the samples of the pass's targets: CONST[kRasterizerRegister] of a text that reads them)
  auto send_constants = [&](uint32_t shader, bool with_samplers) {
    const uint32_t dwords = with_samplers ? kDeviceConstantRegisters * 4 : constants_dwords;
    stream.command(virgl::kSetConstantBuffer, 0, 2 + dwords);
    stream.put(shader);
    stream.put(0);
    for (uint32_t k = 0; k < dwords; ++k) {
      if (k < constants_dwords) {
        stream.put(constants[k]);
      } else if (k < kSamplerCompareRegister * 4) {
        stream.put(0);
      } else if (k >= kRasterizerRegister * 4) {
        stream.put(k == kRasterizerRegister * 4 ? pass_samples : 0u);   // (the pass's samples per pixel, an unsigned integer)
      } else {
        const uint32_t unit = k / 4 - kSamplerCompareRegister;
        float values[4];
        sampler_compare_constant(samplers[unit].compare, samplers[unit].filter == Filter::Linear, values);
        stream.putf(values[k % 4]);
      }
    }
  };
  // The colour targets of the pass that are drawn through their stand-ins (VirglImage::stand_in), and the stand-ins: copied into the targets when the pass ends.
  VirglImage* stood_for[kMaxColorTargets] = {};
  VirglImage* stand_ins[kMaxColorTargets] = {};
  // A straight copy of a whole image into another of its size and format (the host turns the rows over between images kept in the two orders).
  auto copy_image = [&](const VirglImage* from, const VirglImage* to) {
    const Extent2D e = from->extent();
    stream.command(virgl::kBlit, 0, 21);
    stream.put(0xFu);   // (all four channels; nearest, no scissor)
    stream.put(0);
    stream.put(e.width | (e.height << 16));
    stream.put(to->id()), stream.put(0), stream.put(to->blit_format());
    stream.put(0), stream.put(0), stream.put(0), stream.put(e.width), stream.put(e.height), stream.put(1);
    stream.put(from->id()), stream.put(0), stream.put(from->blit_format());
    stream.put(0), stream.put(0), stream.put(0), stream.put(e.width), stream.put(e.height), stream.put(1);
  };
  bool in_pass = false;
  PrimitiveTopology topology = PrimitiveTopology::TriangleList;
  const VirglComputePipeline* compute = nullptr;
  uint32_t patch_points = 0;
  struct StorageBinding {
    VirglBuffer* buffer = nullptr;
    uint64_t offset = 0, size = 0;
  } storage[kMaxStorageBuffers];
  bool compute_constants_dirty = false;
  // Stream output: the buffers bound (kept here) and the host's target objects for them. The objects are made when a draw with stream output first needs them and live to the end of the submit:
  // the host's transform feedback goes on in them from draw to draw (the position in the buffers is the host's) and only ends when they are destroyed; it can only go on with the program it
  // began with, so another program that draws stream output ends it (and the buffers start again where they were bound). It makes the draws of other programs fail unless the targets are not
  // set (no targets) for them. The primitives written are counted by queries of our own to know how far each buffer got, for the next bind that appends.
  struct StreamOutputSlot {
    VirglBuffer* buffer = nullptr;
    uint64_t offset = 0;
    uint64_t end = kStreamOutputNoEnd;   // (where the writes stop: the binding's end, or the buffer's)
  } so_slot[kMaxStreamOutputBuffers];
  uint32_t so_handles[kMaxStreamOutputBuffers] = {};
  uint32_t so_program[5] = {};   // (the shaders of the pipeline the transform feedback began with)
  uint32_t so_query_slot[kMaxGeometryStreams] = {};   // (0: none, else the slot of our query of the primitives the stream wrote)
  struct Fill {
    VirglBuffer* buffer = nullptr;
    uint64_t base = 0;
    uint32_t stride = 0, vertices = 0, query = 0;   // (query 0: not counted)
  } fills[kFillQueries];
  uint32_t fill_count = 0, internal_queries = 0;
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

  // A stage's storage as its module's TgsiBindings number it: the host's shader made for what is bound (a variant, for a module with images or views of buffers), SET_SHADER_BUFFERS of
  // its buffers, SET_SHADER_IMAGES of its images (a texture's level and layers, or a buffer: the host's image is the whole buffer, the view's first element and count are the variant's),
  // and SET_SAMPLER_VIEWS of its buffer views from kTgsiTexelViewBase (a host view of the elements from the buffer's start to the view's last: the first element is the variant's) with
  // the default sampler state. After the stage's textures (which the host sets as the units up to the last, forgetting the views after them).
  auto bind_stage = [&](uint32_t stage, const VirglShaderModule* m) -> bool {
    if (!m || stage >= 6) return true;
    const TgsiBindings& b = m->bindings();
    if (m->has_images()) {
      VirglShaderModule::VariantKey key;
      for (uint32_t k = 0; k < b.image_count && k < kTgsiMaxImages; ++k) {
        const uint32_t slot = b.images[k];
        const TexelBinding& t = texels[slot];
        if (t.buffer) {
          key.formats[k] = texel_virgl_format(t.view.format);
          key.image_first[k] = static_cast<uint32_t>(t.view.offset / texel_bytes(t.view.format));
          key.image_count[k] = t.view.elements;
        } else {
          key.formats[k] = images[slot] ? images[slot]->image_format(image_views[slot].format) : virgl::kFormatR8G8B8A8Unorm;
        }
      }
      for (uint32_t k = 0; k < b.texel_count && k < kTgsiMaxTexelViews; ++k) {
        const TexelBinding& t = texels[b.texels[k]];
        if (!t.buffer) continue;
        key.texel_first[k] = static_cast<uint32_t>(t.view.offset / texel_bytes(t.view.format));
        key.texel_count[k] = t.view.elements;
      }
      const uint32_t shader = m->variant(*this, key);
      if (!shader) return false;
      if (shader != bound_shader[stage]) {
        stream.command(virgl::kBindShader, 0, 2);
        stream.put(shader);
        stream.put(stage);
        bound_shader[stage] = shader;
      }
    }
    if (b.storage_count) {
      stream.command(virgl::kSetShaderBuffers, 0, 2 + 3 * b.storage_count);
      stream.put(stage);
      stream.put(0);
      for (uint32_t k = 0; k < b.storage_count; ++k) {
        const StorageBinding& s = storage[b.storage[k] < kMaxStorageBuffers ? b.storage[k] : 0];
        stream.put(static_cast<uint32_t>(s.offset));
        stream.put(static_cast<uint32_t>(s.size));
        stream.put(s.buffer ? s.buffer->id() : 0);
      }
    }
    if (b.image_count) {
      stream.command(virgl::kSetShaderImages, 0, 2 + 5 * b.image_count);
      stream.put(stage);
      stream.put(0);
      for (uint32_t k = 0; k < b.image_count; ++k) {
        const uint32_t slot = b.images[k] < kMaxStorageImages ? b.images[k] : 0;
        const TexelBinding& t = texels[slot];
        if (t.buffer) {
          stream.put(texel_virgl_format(t.view.format));
          stream.put(virgl::kImageAccessReadWrite);
          stream.put(0);
          stream.put(static_cast<uint32_t>(t.buffer->size()));
          stream.put(t.buffer->id());
          continue;
        }
        const StorageImageView& v = image_views[slot];
        stream.put(images[slot] ? images[slot]->image_format(v.format) : 0);
        stream.put(virgl::kImageAccessReadWrite);
        stream.put(images[slot] ? v.first_layer | ((v.first_layer + v.layer_count - 1) << 16) : 0);
        stream.put(images[slot] ? v.level : 0);
        stream.put(images[slot] ? images[slot]->id() : 0);
      }
    }
    if (b.texel_count) {
      uint32_t handles[kTgsiMaxTexelViews] = {};
      for (uint32_t k = 0; k < b.texel_count && k < kTgsiMaxTexelViews; ++k) {
        TexelBinding& t = texels[b.texels[k] < kMaxImageSlots ? b.texels[k] : kMaxStorageImages];
        if (!t.buffer) continue;
        if (!t.host_view) {
          if (made_views.count == sizeof made_views.handles / sizeof made_views.handles[0]) {
            // (out of room: what was recorded goes out, the views go, and those still bound are made again as they are needed)
            if (!flush()) return false;
            made_views.destroy_all();
            for (TexelBinding& other : texels) other.host_view = 0;
          }
          const uint32_t last = static_cast<uint32_t>(t.view.offset / texel_bytes(t.view.format)) + t.view.elements - 1;
          const uint32_t view = new_handle();
          const uint32_t create[] = {virgl::header(virgl::kCreateObject, virgl::kObjectSamplerView, 6), view, t.buffer->id(), texel_virgl_format(t.view.format) /* | PIPE_BUFFER << 24 */, 0, last,
                                     virgl::kSwizzleRgba};
          if (!send_commands(create, 7)) return false;
          made_views.handles[made_views.count++] = view;
          t.host_view = view;
        }
        handles[k] = t.host_view;
      }
      const uint32_t state = sampler_state(SamplerDesc());
      if (!state) return false;
      stream.command(virgl::kSetSamplerViews, 0, 2 + b.texel_count);
      stream.put(stage);
      stream.put(kTgsiTexelViewBase);
      for (uint32_t k = 0; k < b.texel_count; ++k) stream.put(handles[k]);
      stream.command(virgl::kBindSamplerStates, 0, 2 + b.texel_count);
      stream.put(stage);
      stream.put(kTgsiTexelViewBase);
      for (uint32_t k = 0; k < b.texel_count; ++k) stream.put(state);
    }
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
    so_alive = false;   // (the stream output targets' segments end with it, and none begins until the next)
    for (uint32_t k = 0; k < kMaxGeometryStreams; ++k) {
      if (!so_query_slot[k]) continue;
      end_user(so_query_slot[k]);
      so_query_slot[k] = 0;
    }
    for (uint32_t t = kTargetWritten; t < kQueryTargets; ++t) end_segment(t);
    for (uint32_t& handle : so_handles) {
      if (!handle) continue;
      stream.command(virgl::kDestroyObject, 0, 1);
      stream.put(handle);
      handle = 0;
    }
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
        // (a buffer that is full - an append after the records that filled it - takes nothing more; GL refuses a range of no bytes, so the target is its last 4 bytes, where no record of
        // more than 4 bytes fits: the primitives are needed and not written, as for a full buffer. Records of 4 bytes or less would be written there: refused.)
        const uint64_t limit = so_slot[k].end == kStreamOutputNoEnd || so_slot[k].end > so_slot[k].buffer->size() ? so_slot[k].buffer->size() : so_slot[k].end;
        uint64_t offset = so_slot[k].offset, room = limit > offset ? limit - offset : 0;
        if (room < 4) {
          const uint64_t record = uint64_t{pd.stream_output_stride[k]} * vertices;
          if (record <= 4 || limit < 4) return false;
          offset = limit - 4, room = 4;
        }
        so_handles[k] = new_handle();
        stream.command(virgl::kCreateObject, virgl::kObjectStreamoutTarget, 4);
        stream.put(so_handles[k]), stream.put(so_slot[k].buffer->id()), stream.put(static_cast<uint32_t>(offset)), stream.put(static_cast<uint32_t>(room));
      }
      // The primitives a stream wrote, for each stream the declaration takes buffers from.
      for (uint32_t b = 0; b < kMaxStreamOutputBuffers; ++b) {
        if (!so_slot[b].buffer || !(pipeline->stream_output_buffers() >> b & 1)) continue;
        if (fill_count >= kFillQueries) return false;
        Fill& fill = fills[fill_count++];
        fill = {so_slot[b].buffer, so_slot[b].offset, pd.stream_output_stride[b], vertices, 0};
        const uint32_t output_stream = pipeline->stream_of_buffer(b);
        if (!so_query_slot[output_stream]) {
          if (internal_queries >= kFillQueries) return false;
          const uint32_t slot = kMaxOcclusionQueries + internal_queries++;
          if (!begin_user(slot, QueryKind::SoPrimitivesWritten, output_stream)) return false;
          so_query_slot[output_stream] = slot;
        }
        fill.query = so_query_slot[output_stream];
      }
      for (uint32_t k = 0; k < 5; ++k) so_program[k] = program[k];
      so_alive = true;
      if (!resume_queries()) return false;   // (the stream output targets' segments of the queries open: they count from here)
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
        if (color->samples() != pass_samples) sampler_constants_dirty = true;   // (the samples a text reads with the comparisons)
        pass_samples = color->samples();
        if (!color->surface() || color->format() == Format::D32Float) return false;
        if (depth && (depth->format() != Format::D32Float || !depth->surface())) return false;
        // (the pass is the size of the targets' levels: all of one size)
        if (cmd.pass.level[0] >= color->mip_levels() || (depth && cmd.pass.level[kMaxColorTargets] >= depth->mip_levels())) return false;
        const Extent2D e = {mip_extent(color->extent().width, cmd.pass.level[0]), mip_extent(color->extent().height, cmd.pass.level[0])};
        auto sized = [&](const VirglImage* image, uint32_t level) { return level < image->mip_levels() && mip_extent(image->extent().width, level) == e.width && mip_extent(image->extent().height, level) == e.height; };
        if (depth && !sized(depth, cmd.pass.level[kMaxColorTargets])) return false;
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
          if (!extra->surface() || extra->format() == Format::D32Float || extra == color || extra == depth || extra->y0_top() != color->y0_top() || !sized(extra, cmd.pass.level[n + 1]))
            return false;
          for (uint32_t k = 0; k < n; ++k)
            if (extra_colors[k] == extra) return false;
          target_count = n + 2;
        }
        for (uint32_t n = target_count - 1; n + 1 < kMaxColorTargets; ++n) extra_colors[n] = nullptr;
        // (a depth buffer that is read as a texture keeps its rows the way textures do: colour targets kept the other way are drawn through stand-ins - VirglImage::stand_in -,
        // filled from them first unless the pass clears them)
        if (depth && depth->sampled_depth() && color->y0_top()) {
          for (uint32_t n = 0; n < target_count; ++n) {
            VirglImage*& target = n == 0 ? color : extra_colors[n - 1];
            VirglImage* stand = target->stand_in();
            if (!stand || !stand->surface()) return false;
            if (!cmd.pass.clear_color) copy_image(target, stand);
            stood_for[n] = target;
            stand_ins[n] = stand;
            target = stand;
          }
        }
        // (the surfaces of the layers the pass draws into: a layered one takes the primitives' gl_Layer)
        uint32_t surfaces[kMaxColorTargets + 1] = {};
        for (const VirglImage*& t : pass_target) t = nullptr;
        for (uint32_t n = 0; n <= kMaxColorTargets; ++n) {
          VirglImage* image = n == kMaxColorTargets ? depth : n == 0 ? color : n < target_count ? extra_colors[n - 1] : nullptr;
          if (!image) continue;
          surfaces[n] = cmd.pass.level[n] == 0 && cmd.pass.first_layer[n] == 0 && cmd.pass.layer_count[n] == 1 ? image->surface()
                                                                                                                 : image->surface(cmd.pass.level[n], cmd.pass.first_layer[n], cmd.pass.layer_count[n]);
          if (!surfaces[n]) return false;
          pass_target[n] = image;
          pass_level[n] = cmd.pass.level[n], pass_first[n] = cmd.pass.first_layer[n], pass_count[n] = cmd.pass.layer_count[n];
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
        unset_stream_output();
        in_pass = false;
        for (uint32_t n = 0; n < kMaxColorTargets; ++n) {   // (what was drawn through stand-ins goes into the targets)
          if (stand_ins[n]) copy_image(stand_ins[n], stood_for[n]);
          stand_ins[n] = stood_for[n] = nullptr;
        }
        for (const VirglImage*& t : pass_target) t = nullptr;
        break;
      case RecordedCommand::Type::Resolve: {
        // A blit from the multisampled image to the plain one; one kept upright (an image that is only drawn into) is turned over.
        VirglImage* to = static_cast<VirglImage*>(cmd.image);
        const VirglImage* from = static_cast<const VirglImage*>(cmd.image2);
        if (in_pass || !to || !from || to->samples() != 1 || from->samples() < 2 || to->extent().width != from->extent().width || to->extent().height != from->extent().height ||
            cmd.first >= from->layer_count())
          return false;
        const Extent2D e = from->extent();
        const bool depth = to->format() == Format::D32Float;
        const ResolveMode mode = static_cast<ResolveMode>(cmd.count);
        if (cmd.first != 0 && (mode == ResolveMode::Min || mode == ResolveMode::Max)) return false;   // (the passes of texel fetches read layer 0)
        if (!depth && (mode == ResolveMode::Min || mode == ResolveMode::Max)) {
          // (the least or greatest samples of a colour image - kFeature2ColorResolveModes: the host's blit only averages - drawn, and the state it took set again by
          // the next pass and draw)
          unset_stream_output();
          if (!extreme_resolve(*this, stream, *to, *const_cast<VirglImage*>(from), mode == ResolveMode::Max)) return false;
          pipeline = nullptr;
          textures_dirty = true;
          constants_dirty = constants_dwords != 0;
          bound_shader[virgl::kShaderVertex] = bound_shader[virgl::kShaderFragment] = 0;   // (the resolve bound its own)
          break;
        }
        // (the host's blit averages colour and takes the first sample of depth: the other modes of a depth image are not made)
        if (mode != (depth ? ResolveMode::Sample0 : ResolveMode::Average)) return false;
        const bool flip = !depth && to->y0_top();   // (a multisampled image does not take the flag: the host stores it the way GL does, and only a destination kept upright needs the blit to turn it over)
        pause_queries();
        stream.command(virgl::kBlit, 0, 21);
        stream.put(depth ? (to->has_stencil() ? 0x30u : 0x10u) : 0xFu);   // (all four channels, or depth and stencil; nearest, no scissor)
        stream.put(0);
        stream.put(e.width | (e.height << 16));
        stream.put(to->id()), stream.put(0), stream.put(to->blit_format());
        stream.put(0), stream.put(flip ? e.height : 0), stream.put(0), stream.put(e.width), stream.put(flip ? static_cast<uint32_t>(-static_cast<int32_t>(e.height)) : e.height), stream.put(1);
        stream.put(from->id()), stream.put(0), stream.put(from->blit_format());
        stream.put(0), stream.put(0), stream.put(cmd.first), stream.put(e.width), stream.put(e.height), stream.put(1);   // (the source's layer: resolve_layer)
        if (!resume_queries()) return false;
        break;
      }
      case RecordedCommand::Type::BindComputePipeline: {
        compute = static_cast<const VirglComputePipeline*>(cmd.pipeline);
        if (in_pass || !compute) return false;
        stream.command(virgl::kBindShader, 0, 2);
        stream.put(compute->shader());
        stream.put(virgl::kShaderCompute);
        bound_shader[virgl::kShaderCompute] = compute->shader();   // (0 for a shader with images: its variant goes with the dispatch)
        compute_constants_dirty = constants_dwords != 0;
        compute_textures_changed = true;   // (its comparisons, if it reads them, are sent with its first dispatch)
        compute_bindings_dirty = true;
        break;
      }
      case RecordedCommand::Type::BindStreamOutput: {
        if (cmd.unit >= kMaxStreamOutputBuffers) return false;
        VirglBuffer* target = static_cast<VirglBuffer*>(cmd.buffer);
        const bool append = cmd.offset == kStreamOutputAppend;
        if (append && target && so_slot[cmd.unit].buffer == target && so_slot[cmd.unit].end == cmd.end) break;   // (it goes on where it is)
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
        so_slot[cmd.unit] = {target, start, cmd.end};
        use(target);
        break;
      }
      case RecordedCommand::Type::BindStorageImage: {
        if (cmd.unit >= kMaxStorageImages) return false;
        VirglImage* image = static_cast<VirglImage*>(cmd.image);
        const StorageImageView& v = cmd.storage_view;
        if (image && (!(image->usage() & kImageStorage) || v.level >= image->mip_levels() || v.layer_count == 0 || v.first_layer >= image->level_layers(v.level) ||
                      v.layer_count > image->level_layers(v.level) - v.first_layer ||
                      !storage_view_compatible(image->format(), v.format)))
          return false;
        images[cmd.unit] = image;
        image_views[cmd.unit] = v;
        texels[cmd.unit] = TexelBinding();
        draw_bindings_dirty = compute_bindings_dirty = true;
        break;
      }
      case RecordedCommand::Type::BindStorageTexelBuffer: {
        // (a typed view of a buffer: a format the host has - one that can be stored for a u# slot -, a whole number of elements in the buffer)
        if (cmd.unit >= kMaxImageSlots) return false;
        VirglBuffer* buffer = static_cast<VirglBuffer*>(cmd.buffer);
        const TexelBufferView& v = cmd.texel_view;
        if (buffer) {
          const uint32_t bytes = texel_bytes(v.format);
          if (!texel_format_valid(v.format) || !texel_virgl_format(v.format) || !texel_host_buffer_format(v.format) || (cmd.unit < kMaxStorageImages && !texel_format_storable(v.format)) ||
              v.elements == 0 || v.offset % bytes ||
              v.offset > buffer->size() || uint64_t{v.elements} * bytes > buffer->size() - v.offset)
            return false;
          use(buffer);
        }
        texels[cmd.unit] = {buffer, v, 0};
        if (cmd.unit < kMaxStorageImages) images[cmd.unit] = nullptr;
        draw_bindings_dirty = compute_bindings_dirty = true;
        break;
      }
      case RecordedCommand::Type::BindStorageBuffer:
        if (cmd.unit >= kMaxStorageBuffers || !cmd.buffer) return false;   // (the bindings may change between draws)
        storage[cmd.unit] = {static_cast<VirglBuffer*>(cmd.buffer), cmd.offset, cmd.count};
        use(storage[cmd.unit].buffer);
        draw_bindings_dirty = compute_bindings_dirty = true;
        break;
      case RecordedCommand::Type::Dispatch: {
        if (in_pass || !compute) return false;
        if (compute_textures_dirty) {
          // The textures bound (a prefix of the units, up to the last one bound: the kernel has checked that those the shader reads are there), for the compute stage.
          uint32_t unit_count = 0;
          for (uint32_t unit = 0; unit < kMaxTextureUnits; ++unit)
            if (textures[unit]) unit_count = unit + 1;
          if (unit_count) {
            uint32_t states[kMaxTextureUnits] = {};
            for (uint32_t unit = 0; unit < unit_count; ++unit)
              if (textures[unit] && !(states[unit] = sampler_state(unfiltered_for(samplers[unit], textures[unit])))) return false;
            bool depth_views = false;   // (see the draws': the host's depth texture mode is set as a view is bound)
            for (uint32_t unit = 0; unit < unit_count; ++unit) depth_views = depth_views || (textures[unit] && textures[unit]->sampled_depth());
            if (depth_views) {
              stream.command(virgl::kSetSamplerViews, 0, 2 + unit_count);
              stream.put(virgl::kShaderCompute);
              stream.put(0);
              for (uint32_t unit = 0; unit < unit_count; ++unit) stream.put(0);
            }
            stream.command(virgl::kSetSamplerViews, 0, 2 + unit_count);
            stream.put(virgl::kShaderCompute);
            stream.put(0);
            for (uint32_t unit = 0; unit < unit_count; ++unit) stream.put(textures[unit] ? texture_views[unit] : 0);
            stream.command(virgl::kBindSamplerStates, 0, 2 + unit_count);
            stream.put(virgl::kShaderCompute);
            stream.put(0);
            for (uint32_t unit = 0; unit < unit_count; ++unit) stream.put(states[unit]);
          }
          compute_textures_dirty = false;
          compute_bindings_dirty = true;   // (the buffer views after the textures)
        }
        if (compute_constants_dirty || (compute->module()->sampler_constants() && compute_textures_changed)) {
          send_constants(virgl::kShaderCompute, compute->module()->sampler_constants());
          compute_constants_dirty = false;
          compute_textures_changed = false;
        }
        if (compute_bindings_dirty) {
          // (the host's shader for what is bound, and the buffers, images and buffer views as the shader numbers them)
          if (!bind_stage(virgl::kShaderCompute, compute->module())) return false;
          compute_bindings_dirty = false;
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
      case RecordedCommand::Type::BeginQuery:
        if (cmd.unit >= kMaxOcclusionQueries || (cmd.count & 0xFF) > static_cast<uint32_t>(QueryKind::PipelineStatistics) ||
            !begin_user(cmd.unit, static_cast<QueryKind>(cmd.count & 0xFF), cmd.count >> 8 & 3))
          return false;
        break;
      case RecordedCommand::Type::EndQuery:
        if (!end_user(cmd.count ? cmd.unit : last_open_slot())) return false;
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
          stream.put(pipeline->stencil_reference() | (uint32_t{pipeline->stencil_back_reference()} << 8));
        }
        stream.command(virgl::kBindObject, virgl::kObjectVertexElements, 1);
        stream.put(pipeline->elements());
        stream.command(virgl::kBindShader, 0, 2);
        stream.put(pipeline->vertex_shader());
        stream.put(virgl::kShaderVertex);
        stream.command(virgl::kBindShader, 0, 2);
        stream.put(pipeline->fragment_shader());
        stream.put(virgl::kShaderFragment);
        bound_shader[virgl::kShaderVertex] = pipeline->vertex_shader();
        bound_shader[virgl::kShaderFragment] = pipeline->fragment_shader();
        bound_shader[virgl::kShaderGeometry] = pipeline->geometry_shader();
        bound_shader[virgl::kShaderTessControl] = pipeline->hull_shader();
        bound_shader[virgl::kShaderTessEval] = pipeline->domain_shader();
        draw_bindings_dirty = true;
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
        sampler_constants_dirty = true;
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
        VirglImage* image = static_cast<VirglImage*>(cmd.image);
        if (cmd.unit >= kMaxTextureUnits || !image || !image->view()) return false;   // (only an image created with kImageSampled has a view)
        // (a view of its levels and layers and of the kind the shader declares, or the whole of it)
        TextureView range{image->kind(), 0, image->mip_levels(), 0, image->layer_count()};
        uint32_t handle = image->view(cmd.sampler.stencil);   // (of a depth buffer: its depth, or its stencil plane)
        if (cmd.has_view) {
          range = cmd.texture_view;
          if (range.level_count == 0 && range.base_level < image->mip_levels()) range.level_count = image->mip_levels() - range.base_level;
          if (range.layer_count == 0 && range.first_layer < image->layer_count()) range.layer_count = image->layer_count() - range.first_layer;
          handle = image->view(cmd.texture_view, cmd.sampler.stencil);
          if (!handle) return false;
        }
        textures[cmd.unit] = image;
        texture_views[cmd.unit] = handle;
        texture_ranges[cmd.unit] = range;
        samplers[cmd.unit] = cmd.sampler;
        textures_dirty = true;
        compute_textures_dirty = true;
        sampler_constants_dirty = true;
        compute_textures_changed = true;
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

        if (const uint32_t unit_count = pipeline->desc().texture_count) {
          for (uint32_t unit = 0; unit < unit_count; ++unit)
            if (!textures[unit] || drawn_into(unit)) return false;   // unbound, or a view of what is being drawn into (other levels and layers of it may be sampled)
          if (textures_dirty) {
            // (to the vertex and the pixel stage alike: a vertex shader may read the units too, numbered as the pipeline's)
            uint32_t states[kMaxTextureUnits];
            for (uint32_t unit = 0; unit < unit_count; ++unit) {
              states[unit] = sampler_state(unfiltered_for(samplers[unit], textures[unit]));   // (created straight away, so it exists before the stream that uses it)
              if (!states[unit]) return false;
            }
            // (and to the geometry, control and evaluation stages the pipeline has: a texture read there, by the same numbers)
            bool depth_views = false;   // (a depth buffer's plane is the host texture's mode, set as a view is bound: unbound first, so that binding the view again sets it)
            for (uint32_t unit = 0; unit < unit_count; ++unit) depth_views = depth_views || textures[unit]->sampled_depth();
            for (const uint32_t stage : {virgl::kShaderVertex, virgl::kShaderFragment, virgl::kShaderGeometry, virgl::kShaderTessControl, virgl::kShaderTessEval}) {
              if (!pipeline->stage_module(stage))   // (the stages the pipeline has)
                continue;
              if (depth_views) {
                stream.command(virgl::kSetSamplerViews, 0, 2 + unit_count);
                stream.put(stage);
                stream.put(0);
                for (uint32_t unit = 0; unit < unit_count; ++unit) stream.put(0);
              }
              stream.command(virgl::kSetSamplerViews, 0, 2 + unit_count);
              stream.put(stage);
              stream.put(0);
              for (uint32_t unit = 0; unit < unit_count; ++unit) stream.put(texture_views[unit]);
              stream.command(virgl::kBindSamplerStates, 0, 2 + unit_count);
              stream.put(stage);
              stream.put(0);
              for (uint32_t unit = 0; unit < unit_count; ++unit) stream.put(states[unit]);
            }
            textures_dirty = false;
            draw_bindings_dirty = true;   // (the buffer views after the textures)
          }
        }
        if (draw_bindings_dirty) {
          // The storage of each stage the pipeline has, as its shader numbers it (and the host's shader for what is bound, for one with images); the compute shader's go with a dispatch.
          for (const uint32_t stage : {virgl::kShaderVertex, virgl::kShaderFragment, virgl::kShaderGeometry, virgl::kShaderTessControl, virgl::kShaderTessEval})
            if (!bind_stage(stage, pipeline->stage_module(stage))) return false;
          draw_bindings_dirty = false;
          compute_bindings_dirty = true;   // (the compute stage's own are made again before a dispatch)
        }
        const bool with_samplers = pipeline->sampler_constants();
        if (constants_dirty || (with_samplers && sampler_constants_dirty)) {
          // (the vertex and fragment shaders' stages, and those of the geometry, hull and domain shaders the pipeline has: shader types 2, 3 and 4)
          for (uint32_t shader = 0; shader < 5; ++shader) {
            if (!pipeline->stage_module(shader)) continue;
            send_constants(shader, with_samplers);
          }
          constants_dirty = false;
          if (with_samplers) sampler_constants_dirty = false;
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
        const bool patches = topology == PrimitiveTopology::PatchList;
        // (a stand-in for no pixel shader: the host counts its runs, the API none - the pixel shader invocations' segment stops around the draw)
        constexpr uint32_t kPsTarget = kTargetStatistic + static_cast<uint32_t>(PipelineStatistic::PsInvocations);
        const bool pause_ps = pipeline->desc().stand_in_fragment && queries.active[kPsTarget] >= 0;
        if (pause_ps) end_segment(kPsTarget);
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
        if (pause_ps && queries.open_users[kPsTarget] && !begin_segment(kPsTarget)) return false;
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
  if (in_pass || queries.open_count || !flush()) return false;
  // Ask the host for each query's count (waiting for the draws), then read what it wrote into the buffer.
  for (const HostQuery& q : queries.host.view()) {
    stream.command(virgl::kGetQueryResult, 0, 2);
    stream.put(q.object);
    stream.put(1);
    if (stream.count() > Stream::kCapacity - 512 && !flush()) return false;
  }
  const bool asked = queries.host.count() != 0;
  if (asked && !flush()) return false;
  // (the host writes each result - state, size, count - straight into the buffer's guest memory when its GL says the query is complete. It checks as it polls, which it does while
  // commands with fences are coming in: so keep it busy with empty ones until every state reads DONE. Pulling a buffer from the host would overwrite what it wrote.)
  for (uint32_t attempt = 0; asked; ++attempt) {
    bool pending = false;
    for (const HostQuery& q : queries.host.view()) {
      ByteReader in(q.result->map());
      if (in.le32() != virgl::kQueryStateDone) pending = true;
    }
    if (!pending) break;
    const uint32_t nop = 0;
    if (attempt > kQueryPolls || !send_commands(&nop, 1, /*fence=*/true)) return false;   // (a fenced one: the host looks at its queries while it waits for fences)
  }
  if (disjoint_asked) clock_interrupted_ = false;
  auto host_value = [&](uint32_t index, uint64_t* value) {
    ByteReader in(queries.host[index].result->map());
    in.le32();   // (the state, DONE)
    in.le32();   // (the size of the result: 4 for a sample count, 8 for a time)
    const uint64_t low = in.le32();
    const uint64_t high = in.le32();
    *value = low | (high << 32);
    return in.ok();
  };
  // (what a query of ours counted of a target: its segments of the target added up)
  auto target_sum = [&](const UserQuery& u, uint32_t target, uint64_t* sum) {
    *sum = 0;
    for (uint64_t k = u.first[target]; k < u.end[target] && k < queries.segments.count(); ++k) {
      if (queries.segments[k].target != target) continue;
      uint64_t v = 0;
      if (!host_value(queries.segments[k].host, &v)) return false;
      *sum += v;
    }
    return true;
  };
  uint64_t counts[kQuerySlots] = {};
  for (uint32_t slot = 0; slot < kQuerySlots; ++slot) {
    if (queries.slot_user[slot] < 0) continue;
    const UserQuery& u = queries.users[static_cast<uint64_t>(queries.slot_user[slot])];
    uint64_t values[kMaxQueryValues] = {};
    if (u.synthetic) {
      values[0] = u.synthetic_value;
    } else if (u.kind == QueryKind::Timestamp) {
      if (!host_value(u.timestamp, &values[0])) return false;
    } else if (u.kind == QueryKind::PipelineStatistics) {
      for (uint32_t k = 0; k < kPipelineStatisticCount; ++k)
        if (!target_sum(u, kTargetStatistic + k, &values[k])) return false;
    } else {
      uint32_t target = 0;
      while (target < kQueryTargets && !(u.targets >> target & 1)) ++target;
      if (target == kQueryTargets || !target_sum(u, target, &values[0])) return false;
      if (u.kind == QueryKind::OcclusionPredicate) values[0] = values[0] ? 1 : 0;
    }
    counts[slot] = values[0];
    if (slot < kMaxOcclusionQueries) recorder.set_query_values(slot, ArrayView<const uint64_t>(values));
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
