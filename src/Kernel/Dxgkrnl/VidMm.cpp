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

#include "Kernel/Dxgkrnl/VidMm.hpp"
#include "Kernel/Dxgkrnl/D3d12.hpp"
#include "Kernel/Dxgkrnl/CommandStream.hpp"
#include "Graphics/Graphics3D/Mipmaps.hpp"
#include "Graphics/WidePixels.hpp"
#include "Graphics/BlockCompression.hpp"
#include "Utility/AllocatedView.hpp"
#include "Utility/ByteStream.hpp"

namespace dxgk {

namespace {

// The device's format of an allocation format that is read and written as Vec4s.
graphics::Format wide_format_of(Format f) {
  switch (f) {
    case Format::A16B16G16R16F: return graphics::Format::R16G16B16A16Float;
    case Format::A32B32G32R32F: return graphics::Format::R32G32B32A32Float;
    case Format::A16B16G16R16: return graphics::Format::R16G16B16A16Unorm;
    case Format::A32B32G32R32Uint: return graphics::Format::R32G32B32A32Uint;
    case Format::A32B32G32R32Sint: return graphics::Format::R32G32B32A32Sint;
    case Format::A16B16G16R16Uint: return graphics::Format::R16G16B16A16Uint;
    case Format::A16B16G16R16Sint: return graphics::Format::R16G16B16A16Sint;
    case Format::A8B8G8R8Uint: return graphics::Format::R8G8B8A8Uint;
    case Format::A16B16G16R16Snorm: return graphics::Format::R16G16B16A16Snorm;
    case Format::A2B10G10R10: return graphics::Format::R10G10B10A2Unorm;
    case Format::R32Uint: return graphics::Format::R32Uint;
    case Format::R32Sint: return graphics::Format::R32Sint;
    case Format::R32Float: return graphics::Format::R32Float;
    default: return graphics::Format::Undefined;
  }
}

}  // namespace

graphics::Format device_wide_format(Format f) { return wide_format_of(f); }
bool storage_view_format(Format allocation, Format view, graphics::Format* out) {
  *out = graphics::Format::Undefined;
  if (view == Format::Unknown || view == allocation) return true;
  // (the 8-bit RGBA image as integers, and the integer one as normalised values: the same bytes)
  if ((allocation == Format::A8B8G8R8 || allocation == Format::X8B8G8R8) && view == Format::A8B8G8R8Uint) return *out = graphics::Format::R8G8B8A8Uint, true;
  if (allocation == Format::A8B8G8R8Uint && view == Format::A8B8G8R8) return *out = graphics::Format::R8G8B8A8Unorm, true;
  if (!dxgk::is_wide(allocation) || !dxgk::is_wide(view) || pixel_bytes(allocation) != pixel_bytes(view)) return false;
  *out = wide_format_of(view);
  return true;
}

namespace {

constexpr uint64_t kMaxQueryResultBytes = 1 << 24;   // (131072 Direct3D 12 pipeline statistics queries of 112 bytes fit)
constexpr uint32_t kKnownFlags = kAllocVertexBuffer | kAllocIndexBuffer | kAllocTexture | kAllocRenderTarget | kAllocDepthStencil | kAllocPresent | kAllocStaging | kAllocShared | kAllocQueryResult | kAllocUnorderedAccess | kAllocStreamOutput | kAllocConstantBuffer | kAllocTrackState | kAllocHeap | kAllocReserved | kAllocStorageImage;

// The bytes of all levels of a w x h image of format `f` (pixels, or blocks of a compressed format); false if the description's levels are not possible. (w and h are at most
// kMaxTextureDimension, so nothing overflows.)
bool image_bytes(uint32_t w, uint32_t h, uint32_t levels, Format f, uint64_t* total) {
  uint64_t sum = 0;
  for (uint32_t l = 0; l < levels; ++l) sum += image_level_bytes(f, graphics::mip_extent(w, l), graphics::mip_extent(h, l));
  *total = sum;
  return true;
}
// The device's format of a compressed one.
graphics::Format compressed_format_of(Format f) {
  switch (f) {
    case Format::Bc1: return graphics::Format::Bc1Unorm;
    case Format::Bc2: return graphics::Format::Bc2Unorm;
    case Format::Bc3: return graphics::Format::Bc3Unorm;
    case Format::Bc1Srgb: return graphics::Format::Bc1Srgb;
    case Format::Bc2Srgb: return graphics::Format::Bc2Srgb;
    case Format::Bc3Srgb: return graphics::Format::Bc3Srgb;
    default: return graphics::Format::Undefined;
  }
}

// The backing as 32-bit words (it is allocated 16-byte aligned, so this holds; false would mean a bug elsewhere).
bool as_words(ByteView bytes, ArrayView<uint32_t>* out) { return bytes.try_cast<uint32_t>(out); }

}  // namespace

bool Allocation::level_range(uint32_t level, uint64_t* offset, uint64_t* bytes) const {
  if (is_buffer()) {
    if (level != 0) return false;
    *offset = 0;
    *bytes = desc_.size;
    return true;
  }
  if (level >= desc_.mip_levels) return false;
  if (desc_.kind != graphics::TextureKind::Texture2D) {   // (one level: all the layers)
    *offset = 0;
    *bytes = image_level_bytes(desc_.format, desc_.width, desc_.height) * layers();
    return true;
  }
  uint64_t start = 0;
  for (uint32_t l = 0; l < level; ++l) start += image_level_bytes(desc_.format, graphics::mip_extent(desc_.width, l), graphics::mip_extent(desc_.height, l));
  *offset = start;
  *bytes = image_level_bytes(desc_.format, graphics::mip_extent(desc_.width, level), graphics::mip_extent(desc_.height, level));
  return true;
}

Allocation::Allocation(VidMm& manager, allocators::IAllocator& allocator, const AllocationDesc& desc, uint64_t id)
    : manager_(manager), allocator_(allocator), desc_(desc), id_(id), placed_(allocator), tiles_(allocator), mirror_sources_(allocator) {}

Allocation::~Allocation() { manager_.forget(*this); }

VidMm::VidMm(allocators::IAllocator& allocator, graphics::IGraphicsDevice& device, uint64_t local_budget, uint64_t system_limit)
    : allocator_(allocator), device_(device), budget_(local_budget), system_limit_(system_limit), resident_(allocator), scratch_(allocator) {}

VidMm::~VidMm() {}

uint32_t VidMm::to_logical(Format f, uint32_t word) {
  if (is_rgba_order(f)) word = (word & 0xFF00FF00u) | ((word & 0xFF) << 16) | ((word >> 16) & 0xFF);
  return has_alpha(f) ? word : (word | 0xFF000000u);
}

uint32_t VidMm::from_logical(Format f, uint32_t pixel) {
  if (is_rgba_order(f)) pixel = (pixel & 0xFF00FF00u) | ((pixel & 0xFF) << 16) | ((pixel >> 16) & 0xFF);
  return pixel;
}

// --- Reserved textures: the numbering of tiles ----------------------------------------------------------------------------------------------------------------------------
// A reserved texture's texels are `texel_bytes` each in the heaps - its image's pixels, or the narrower ones its tile_narrow names (Direct3D's R8 / R8G8 / R16 / R16G16, which
// the image keeps widened) -, so its tiles have Direct3D's standard shape for that size (tile_shape). The levels that are at least a tile in both directions - the standard ones -
// are cut into tiles, row by row, level after level; a tile of one is its rows one after the other in the heap, each tile_shape().width texels (the part of an edge tile outside
// the level is not used). The smaller levels (the packed mips) come last: their texels one level after the other, row by row, over as many tiles as they fill.
namespace {
uint32_t tile_texel_bytes(const AllocationDesc& d) { return d.tile_narrow ? narrow_pixel_bytes(static_cast<NarrowLayout>(d.tile_narrow)) : pixel_bytes(d.format); }
uint32_t standard_levels(const AllocationDesc& d) {
  const TileShape t = tile_shape(tile_texel_bytes(d));
  uint32_t n = 0;
  while (n < d.mip_levels && graphics::mip_extent(d.width, n) >= t.width && graphics::mip_extent(d.height, n) >= t.height) ++n;
  return n;
}
uint64_t level_tile_columns(const AllocationDesc& d, uint32_t level) {
  const uint32_t across = tile_shape(tile_texel_bytes(d)).width;
  return (graphics::mip_extent(d.width, level) + uint64_t{across} - 1) / across;
}
uint64_t level_tile_rows(const AllocationDesc& d, uint32_t level) {
  const uint32_t down = tile_shape(tile_texel_bytes(d)).height;
  return (graphics::mip_extent(d.height, level) + uint64_t{down} - 1) / down;
}
// The heap bytes of the packed levels, and the tiles they take.
uint64_t packed_bytes(const AllocationDesc& d) {
  uint64_t n = 0;
  for (uint32_t l = standard_levels(d); l < d.mip_levels; ++l) n += uint64_t{graphics::mip_extent(d.width, l)} * graphics::mip_extent(d.height, l) * tile_texel_bytes(d);
  return n;
}
uint64_t packed_tiles(const AllocationDesc& d) { return (packed_bytes(d) + kTileBytes - 1) / kTileBytes; }
uint64_t texture_tile_count(const AllocationDesc& d) {
  const uint32_t standard = standard_levels(d);
  uint64_t n = packed_tiles(d);
  for (uint32_t l = 0; l < standard; ++l) n += level_tile_columns(d, l) * level_tile_rows(d, l);
  return n;
}
struct TileSpot {
  uint32_t level = 0, x = 0, y = 0;   // a standard level's tile: its column and row in the level
  bool packed = false;
  uint64_t packed_index = 0;          // a tile of the packed levels: which of them
};
bool tile_spot(const AllocationDesc& d, uint64_t tile, TileSpot* out) {
  const uint32_t standard = standard_levels(d);
  for (uint32_t l = 0; l < standard; ++l) {
    const uint64_t across = level_tile_columns(d, l), count = across * level_tile_rows(d, l);
    if (tile < count) {
      *out = {l, static_cast<uint32_t>(tile % across), static_cast<uint32_t>(tile / across), false, 0};
      return true;
    }
    tile -= count;
  }
  if (tile < packed_tiles(d)) {
    *out = {standard, 0, 0, true, tile};
    return true;
  }
  return false;
}
// The texels of tile `tile` of a reserved texture: calls use(image_run, heap_at, texels) for each run of them that is contiguous in both - `image_run` the run's pixels in the
// backing, `heap_at` where its texels start in the heap's tile.
template <typename Use>
void for_tile_runs(const Allocation& a, uint64_t tile, ByteView backing, Use use) {
  const AllocationDesc& d = a.desc();
  TileSpot spot;
  if (!tile_spot(d, tile, &spot)) return;
  const uint32_t image_bytes = pixel_bytes(d.format), texel_bytes = tile_texel_bytes(d);
  if (spot.packed) {
    // (the packed levels as one run of texels, of which this tile is the kTileBytes from packed_index * kTileBytes on; texel_bytes divides kTileBytes, so no texel straddles two)
    const uint64_t low = spot.packed_index * kTileBytes, high = low + kTileBytes;
    uint64_t at = 0;
    for (uint32_t l = spot.level; l < d.mip_levels && at < high; ++l) {
      const uint32_t w = graphics::mip_extent(d.width, l), h = graphics::mip_extent(d.height, l);
      const uint64_t row = uint64_t{w} * texel_bytes;
      if (at + row * h <= low) {
        at += row * h;
        continue;
      }
      uint64_t offset = 0, bytes = 0;
      if (!a.level_range(l, &offset, &bytes)) return;
      for (uint32_t y = 0; y < h && at < high; ++y, at += row) {
        const uint64_t from = at > low ? at : low, to = at + row < high ? at + row : high;
        if (from >= to) continue;
        const uint64_t first = (from - at) / texel_bytes, count = (to - from) / texel_bytes;
        ByteView run;
        if (backing.try_subview(offset + (uint64_t{y} * w + first) * image_bytes, count * image_bytes, &run)) use(run, from - low, static_cast<uint32_t>(count));
      }
    }
    return;
  }
  uint64_t offset = 0, bytes = 0;
  if (!a.level_range(spot.level, &offset, &bytes)) return;
  const TileShape shape = tile_shape(texel_bytes);
  const uint32_t level_w = graphics::mip_extent(d.width, spot.level), level_h = graphics::mip_extent(d.height, spot.level);
  const uint32_t x0 = spot.x * shape.width, y0 = spot.y * shape.height;
  const uint32_t columns = level_w - x0 < shape.width ? level_w - x0 : shape.width, rows = level_h - y0 < shape.height ? level_h - y0 : shape.height;
  for (uint32_t row = 0; row < rows; ++row) {
    ByteView run;
    if (backing.try_subview(offset + (uint64_t{y0 + row} * level_w + x0) * image_bytes, uint64_t{columns} * image_bytes, &run))
      use(run, uint64_t{row} * shape.width * texel_bytes, columns);
  }
}
// A run of `texels` of a reserved texture between its image's pixels and the heap's texels: the same bytes, or - a narrow layout - the first channels of each pixel, widened back
// to (r, g or 0, 0, the maximum; 1 for an integer image) as CopyBufferImage widens. An empty `heap` (a tile mapped nowhere) gives zero texels.
void image_from_heap(const AllocationDesc& d, ByteView image, ConstByteView heap, uint32_t texels) {
  if (!d.tile_narrow) {
    if (heap.empty()) image.fill(0);
    else image.copy_from(heap);
    return;
  }
  const NarrowLayout narrow = static_cast<NarrowLayout>(d.tile_narrow);
  const bool sixteen = narrow == NarrowLayout::R16 || narrow == NarrowLayout::R16G16, integer = d.format == Format::A16B16G16R16Sint;
  const uint32_t sample = sixteen ? 2 : 1, pixel = 4 * sample, texel = narrow_pixel_bytes(narrow);
  for (uint32_t i = 0; i < texels; ++i) {
    ByteView to;
    if (!image.try_subview(uint64_t{i} * pixel, pixel, &to)) return;
    to.fill(0);
    ConstByteView from;
    if (!heap.empty() && heap.try_subview(uint64_t{i} * texel, texel, &from)) to.subview(0, texel).copy_from(from);
    if (integer) to[3 * sample] = 1;
    else to.subview(3 * sample, sample).fill(0xFF);
  }
}
void heap_from_image(const AllocationDesc& d, ByteView heap, ConstByteView image, uint32_t texels) {
  if (!d.tile_narrow) {
    heap.copy_from(image);
    return;
  }
  const NarrowLayout narrow = static_cast<NarrowLayout>(d.tile_narrow);
  const uint32_t sample = narrow == NarrowLayout::R16 || narrow == NarrowLayout::R16G16 ? 2 : 1, pixel = 4 * sample, texel = narrow_pixel_bytes(narrow);
  for (uint32_t i = 0; i < texels; ++i) {
    ConstByteView from;
    ByteView to;
    if (!image.try_subview(uint64_t{i} * pixel, texel, &from) || !heap.try_subview(uint64_t{i} * texel, texel, &to)) return;
    to.copy_from(from);
  }
}
}  // namespace

NtStatus VidMm::create(const AllocationDesc& requested, SharedPointer<Allocation>* out, const SharedPointer<Allocation>& heap) {
  AllocationDesc desc = requested;
  const bool track_state = (desc.flags & kAllocTrackState) != 0;   // (the other checks are about what the allocation is, not what states it moves through)
  desc.flags &= ~kAllocTrackState;
  const bool reserved = (desc.flags & kAllocReserved) != 0;
  desc.flags &= ~kAllocReserved;
  // A reserved resource is a buffer of whole tiles, or a plain sampled texture of colour pixels (one sample and layer; its tiles of tile_shape(), its mip levels below a tile in
  // either direction packed at the end, see tile_spot()), whose texels in the heaps may be narrower than its pixels (tile_narrow).
  const bool reserved_texture = reserved && desc.flags == kAllocTexture;
  if (reserved && (heap || requested.heap || (desc.flags & (kAllocQueryResult | kAllocConstantBuffer | kAllocHeap | kAllocShared))) ) return status::kInvalidParameter;
  if (reserved && !reserved_texture && (!(desc.flags & (kAllocVertexBuffer | kAllocIndexBuffer | kAllocUnorderedAccess | kAllocStreamOutput)) || desc.size == 0 || desc.size % kTileBytes)) return status::kInvalidParameter;
  if (desc.tile_narrow != 0) {
    // (a narrow layout: of an 8-bit four-channel image for R8 / R8G8, of a 16-bit one for R16 / R16G16 - what CopyBufferImage widens into)
    const uint32_t n = desc.tile_narrow;
    const bool eight = n == static_cast<uint32_t>(NarrowLayout::R8) || n == static_cast<uint32_t>(NarrowLayout::R8G8);
    const bool sixteen = n == static_cast<uint32_t>(NarrowLayout::R16) || n == static_cast<uint32_t>(NarrowLayout::R16G16);
    if (!reserved_texture || (!eight && !sixteen) || (eight && desc.format != Format::A8B8G8R8 && desc.format != Format::X8B8G8R8) ||
        (sixteen && desc.format != Format::A16B16G16R16 && desc.format != Format::A16B16G16R16Sint))
      return status::kInvalidParameter;
  }
  if (reserved_texture && (!dxgk::is_color(desc.format) || tile_shape(tile_texel_bytes(desc)).width == 0 || desc.width == 0 || desc.height == 0 || desc.mip_levels == 0 ||
                           desc.samples != 1 || desc.kind != graphics::TextureKind::Texture2D || desc.layers != 1))
    return status::kInvalidParameter;
  if (track_state && (requested.initial_state & ~kStateMask)) return status::kInvalidParameter;
  if (desc.flags == 0 || (desc.flags & ~kKnownFlags)) return status::kInvalidParameter;
  const bool buffer = (desc.flags & (kAllocVertexBuffer | kAllocIndexBuffer | kAllocQueryResult | kAllocUnorderedAccess | kAllocStreamOutput | kAllocConstantBuffer | kAllocHeap)) != 0;
  // (a storage image: a sampled colour texture of one sample that is not sRGB, block-compressed or a cube, on a device with them)
  if ((desc.flags & kAllocStorageImage) &&
      (buffer || !(desc.flags & kAllocTexture) || (desc.flags & ~(kAllocStorageImage | kAllocTexture | kAllocRenderTarget | kAllocShared)) || !dxgk::is_color(desc.format) || dxgk::is_srgb(desc.format) ||
       desc.samples != 1 || desc.kind == graphics::TextureKind::TextureCube))
    return status::kInvalidParameter;
  if ((desc.flags & kAllocStorageImage) && !(device_.info().features2 & graphics::kFeature2StorageImages)) return status::kNotSupported;
  if (dxgk::is_single_channel(desc.format) && !(desc.flags & kAllocStaging) && !(device_.info().features2 & graphics::kFeature2SingleChannelFormats)) return status::kNotSupported;
  if ((desc.flags & kAllocHeap) && ((desc.flags & ~kAllocShared) != kAllocHeap || heap || requested.heap)) return status::kInvalidParameter;      // (a heap may be shared: D3D12_HEAP_FLAG_SHARED)
  if ((desc.flags & kAllocUnorderedAccess) && !(device_.info().features & graphics::kFeatureCompute)) return status::kNotSupported;
  if ((desc.flags & kAllocStreamOutput) && !(device_.info().features & graphics::kFeatureStreamOutput)) return status::kNotSupported;
  uint64_t cpu_bytes = 0, device_bytes = 0;
  if (desc.flags & kAllocQueryResult) {
    // (results are written into system memory by the CPU: no device object)
    if (desc.flags & ~(kAllocQueryResult | kAllocShared)) return status::kInvalidParameter;
    if (!(device_.info().features & graphics::kFeatureOcclusionQueries)) return status::kNotSupported;
    if (desc.size < 8 || desc.size > kMaxQueryResultBytes) return status::kInvalidParameter;
    cpu_bytes = desc.size;
  } else if (desc.flags & kAllocHeap) {
    if (desc.size < 256 || desc.size % 256 || desc.size > kMaxBufferBytes) return status::kInvalidParameter;
    cpu_bytes = desc.size;
  } else if (desc.flags & kAllocConstantBuffer) {
    // (read by the CPU when a draw takes the constants: no device object)
    if (desc.flags & ~(kAllocConstantBuffer | kAllocShared)) return status::kInvalidParameter;
    if (desc.size < 16 || desc.size % 16 || desc.size > kMaxBufferBytes) return status::kInvalidParameter;
    cpu_bytes = desc.size;
  } else if (buffer) {
    if (desc.flags & (kAllocTexture | kAllocRenderTarget | kAllocDepthStencil | kAllocPresent | kAllocStaging)) return status::kInvalidParameter;
    if (desc.size == 0 || desc.size > kMaxBufferBytes) return status::kInvalidParameter;
    cpu_bytes = device_bytes = desc.size;
  } else {
    if (desc.width == 0 || desc.height == 0 || desc.width > kMaxTextureDimension || desc.height > kMaxTextureDimension) return status::kInvalidParameter;
    if (desc.mip_levels == 0 || desc.mip_levels > graphics::mip_level_limit({desc.width, desc.height})) return status::kInvalidParameter;
    const bool depth_flag = (desc.flags & kAllocDepthStencil) != 0;
    if (depth_flag != dxgk::is_depth(desc.format)) return status::kInvalidParameter;
    const bool compressed = dxgk::is_compressed(desc.format);
    if (!depth_flag && !dxgk::is_color(desc.format) && !compressed) return status::kInvalidParameter;
    // (block-compressed: sampled only - or staging memory - and of one sample)
    if (compressed && ((desc.flags & ~(kAllocTexture | kAllocShared | kAllocStaging)) || ((desc.flags & kAllocStaging) != 0) == ((desc.flags & kAllocTexture) != 0) || desc.samples != 1))
      return status::kInvalidParameter;
    if (desc.flags & kAllocStaging) {
      if (desc.flags & ~(kAllocStaging | kAllocShared)) return status::kInvalidParameter;
    } else if (!(desc.flags & (kAllocTexture | kAllocRenderTarget | kAllocDepthStencil | kAllocPresent))) {
      return status::kInvalidParameter;
    }
    if (depth_flag && (desc.flags & ~(kAllocDepthStencil | kAllocShared))) return status::kInvalidParameter;
    if (dxgk::is_wide(desc.format)) {
      if (desc.mip_levels != 1 || (desc.flags & kAllocPresent)) return status::kInvalidParameter;
      const uint32_t needed = dxgk::is_single_channel(desc.format) ? 0u : dxgk::is_float(desc.format) ? graphics::kFeatureFloatFormats : dxgk::is_integer(desc.format) ? graphics::kFeatureIntegerFormats : (desc.format == Format::A16B16G16R16Snorm || desc.format == Format::A2B10G10R10) ? graphics::kFeatureSnormFormats : graphics::kFeatureUnorm16Formats;
      if (!(desc.flags & kAllocStaging) && (device_.info().features & needed) != needed) return status::kNotSupported;
    }
    if (dxgk::is_srgb(desc.format)) {
      if (desc.flags & kAllocPresent) return status::kInvalidParameter;
      if (!(desc.flags & kAllocStaging) && !(device_.info().features & graphics::kFeatureSrgbFormats)) return status::kNotSupported;
    }
    if (desc.mip_levels > 1 && (desc.flags & ~(kAllocTexture | kAllocShared | kAllocStorageImage))) return status::kInvalidParameter;
    uint64_t total = 0;
    image_bytes(desc.width, desc.height, desc.mip_levels, desc.format, &total);
    if (desc.samples != 1) {
      // (a multisampled target: the device keeps the samples, the system keeps nothing)
      const graphics::DeviceInfo info = device_.info();
      if (!(info.features & graphics::kFeatureMultisample) || desc.samples >= 32 || !(info.sample_counts >> desc.samples & 1)) return status::kNotSupported;
      if ((depth_flag ? (desc.flags & ~kAllocShared) != kAllocDepthStencil : (desc.flags & ~(kAllocTexture | kAllocShared)) != kAllocRenderTarget) || desc.mip_levels != 1 || desc.kind != graphics::TextureKind::Texture2D || desc.layers != 1 || dxgk::is_wide(desc.format))
        return status::kInvalidParameter;
      device_bytes = uint64_t{desc.width} * desc.height * desc.samples * (depth_flag ? (desc.format == Format::D24S8 ? 5 : 4) : 4);
      goto sized;
    }
    if (desc.kind != graphics::TextureKind::Texture2D) {
      // (a volume, array or cube: one level, sampled - and also drawn into or read and written as a storage image - or a depth buffer that is an array or a cube; of any colour format but
      // the block-compressed ones, which are sampled only; the wide ones the devices take as layers of Vec4s)
      const uint32_t colour_flags = kAllocTexture | kAllocShared | kAllocRenderTarget | kAllocStorageImage;
      if (depth_flag ? (desc.flags & ~(kAllocDepthStencil | kAllocShared)) || desc.kind == graphics::TextureKind::Texture3D
                     : (desc.flags & ~colour_flags) || !(desc.flags & kAllocTexture) || (compressed && (desc.flags & (kAllocRenderTarget | kAllocStorageImage))))
        return status::kInvalidParameter;
      if (desc.mip_levels != 1) return status::kInvalidParameter;
      if (desc.layers == 0 || desc.layers > graphics::kMaxTextureLayers || (desc.kind == graphics::TextureKind::TextureCube && (desc.layers != 6 || desc.width != desc.height))) return status::kInvalidParameter;
      if (!(device_.info().features & graphics::kFeatureTextureKinds)) return status::kNotSupported;
      if ((desc.flags & (kAllocRenderTarget | kAllocDepthStencil)) && !(device_.info().features2 & graphics::kFeature2LayeredTargets)) return status::kNotSupported;
      total *= desc.layers;
    } else if (desc.layers != 1) {
      return status::kInvalidParameter;
    }
    if (depth_flag) {
      device_bytes = uint64_t{desc.width} * desc.height * (desc.format == Format::D24S8 ? 5 : 4) * (desc.kind == graphics::TextureKind::Texture2D ? 1 : desc.layers);
    } else {
      cpu_bytes = total;
      device_bytes = desc.flags & kAllocStaging ? 0 : total;
    }
  }
sized:
  const bool placed = requested.heap != 0;
  if (placed) {
    // (its backing is part of the heap's memory: what can be in a heap are the things whose contents are the backing)
    if (!heap || !heap->is_heap() || &heap->manager() != this || (desc.flags & kAllocShared)) return status::kInvalidParameter;
    if (desc.samples != 1 || (desc.flags & kAllocDepthStencil)) return status::kNotSupported;   // (these have no backing to share)
    if (requested.heap_offset % 16 || requested.heap_offset > heap->desc().size || cpu_bytes == 0 || cpu_bytes > heap->desc().size - requested.heap_offset) return status::kInvalidParameter;
  } else if (cpu_bytes > system_limit_ || system_bytes_ > system_limit_ - cpu_bytes) {
    return status::kGraphicsNoVideoMemory;
  }

  SharedPointer<Allocation> a = SharedPointer<Allocation>::make(allocator_, *this, allocator_, desc, next_id_);
  if (!a) return status::kNoMemory;
  if (placed) {
    if (!heap->placed_.push_back(a.get())) return status::kNoMemory;
    a->heap_ = heap;
    a->heap_offset_ = requested.heap_offset;
    a->cpu_ = heap->cpu_.subview(requested.heap_offset, cpu_bytes);   // (shared, not owned)
    cpu_bytes = 0;                                                    // (and not charged again)
  } else if (cpu_bytes) {
    a->cpu_ = allocate_view<uint8_t>(allocator_, cpu_bytes, 16);
    if (a->cpu_.empty()) return status::kNoMemory;
    a->cpu_.fill(0);
  }
  ++next_id_;
  if (reserved)
    for (uint64_t i = 0, tiles = reserved_texture ? texture_tile_count(desc) : desc.size / kTileBytes; i < tiles; ++i)
      if (!a->tiles_.push_back(Allocation::TileBinding())) return status::kNoMemory;
  a->reserved_ = reserved;
  a->state_tracked_ = track_state;
  a->state_ = track_state ? requested.initial_state : 0;
  a->device_bytes_ = device_bytes;
  system_bytes_ += cpu_bytes;
  *out = a;
  return status::kSuccess;
}

void VidMm::forget(Allocation& a) {
  if (a.resident()) release_device_object(a);
  if (a.heap_) {
    for (uint64_t i = 0; i < a.heap_->placed_.count(); ++i)
      if (a.heap_->placed_[i] == &a) {
        a.heap_->placed_.remove_at(i);
        break;
      }
    return;   // (the memory is the heap's)
  }
  system_bytes_ -= a.cpu_.size();
  release_view(allocator_, a.cpu_);
}

void VidMm::release_device_object(Allocation& a) {
  if (a.buffer_) a.so_filled_ = a.buffer_->stream_output_filled();   // (where the next stream output that appends goes)
  if (a.image_) device_.destroy_image(a.image_);
  if (a.buffer_) device_.destroy_buffer(a.buffer_);
  a.image_ = nullptr;
  a.buffer_ = nullptr;
  resident_bytes_ -= a.device_bytes_;
  for (uint64_t i = 0; i < resident_.count(); ++i)
    if (resident_[i] == &a) {
      resident_.remove_at(i);
      break;
    }
}

bool VidMm::create_device_object(Allocation& a) {
  const AllocationDesc& d = a.desc_;
  if (a.is_buffer()) {
    uint32_t usage = 0;
    if (d.flags & kAllocVertexBuffer) usage |= graphics::kBufferVertex;
    if (d.flags & kAllocIndexBuffer) usage |= graphics::kBufferIndex;
    if (d.flags & kAllocUnorderedAccess) usage |= graphics::kBufferStorage;
    if (d.flags & kAllocStreamOutput) usage |= graphics::kBufferStreamOutput;
    a.buffer_ = device_.create_buffer(d.size, usage);
    if (a.buffer_) a.buffer_->set_stream_output_filled(a.so_filled_);
    return a.buffer_ != nullptr;
  }
  graphics::ImageDesc id;
  id.extent = {d.width, d.height};
  id.mip_levels = d.mip_levels;
  id.kind = d.kind;
  id.layers = d.layers;
  id.samples = d.samples;
  if (a.is_depth()) {
    id.format = graphics::Format::D32Float;
    id.usage = graphics::kImageDepthAttachment;
    id.stencil = d.format == Format::D24S8;
    a.image_ = device_.create_image(id);
    return a.image_ != nullptr;
  }
  // (a compressed texture: as it is on a device that samples it so, else decoded to 8-bit pixels when it is uploaded)
  const bool native_blocks = (device_.info().features & graphics::kFeatureCompressedFormats) != 0;
  id.format = dxgk::is_compressed(d.format) ? (native_blocks ? compressed_format_of(d.format) : graphics::decoded_format(compressed_format_of(d.format)))
              : dxgk::is_wide(d.format) ? wide_format_of(d.format)
              : dxgk::is_srgb(d.format) ? (is_rgba_order(d.format) ? graphics::Format::R8G8B8A8Srgb : graphics::Format::B8G8R8A8Srgb)
              : is_rgba_order(d.format) ? graphics::Format::R8G8B8A8Unorm : graphics::Format::B8G8R8A8Unorm;
  id.usage = 0;
  if (d.flags & kAllocRenderTarget) id.usage |= graphics::kImageColorAttachment;
  if (d.flags & kAllocPresent) id.usage |= graphics::kImagePresent;
  if (d.flags & kAllocTexture) id.usage |= graphics::kImageSampled;
  if (d.flags & kAllocStorageImage) id.usage |= graphics::kImageStorage;
  // (uploading the backing needs kImageSampled on some devices: a render target gets it too, and does without if the device refuses the combination)
  if (d.mip_levels == 1 && id.usage != graphics::kImageSampled) {
    graphics::ImageDesc with_sampled = id;
    with_sampled.usage |= graphics::kImageSampled;
    a.image_ = device_.create_image(with_sampled);
    if (a.image_) return true;
  }
  a.image_ = device_.create_image(id);
  return a.image_ != nullptr;
}

void VidMm::gather_tiles(Allocation& a) {
  if (a.is_image()) {
    for (uint64_t i = 0; i < a.tiles_.count(); ++i) {
      const Allocation::TileBinding& b = a.tiles_[i];
      for_tile_runs(a, i, a.cpu_, [&](ByteView image, uint64_t at, uint32_t texels) {
        ConstByteView from;
        const uint64_t heap_bytes = uint64_t{texels} * tile_texel_bytes(a.desc_);
        if (!b.heap || !b.heap->backing().try_subview(b.offset + at, heap_bytes, &from)) from = ConstByteView();   // (nowhere: zero texels)
        image_from_heap(a.desc_, image, from, texels);
      });
    }
    return;
  }
  for (uint64_t i = 0; i < a.tiles_.count(); ++i) {
    ByteView tile;
    if (!a.cpu_.try_subview(i * kTileBytes, kTileBytes, &tile)) continue;
    const Allocation::TileBinding& b = a.tiles_[i];
    ConstByteView from;
    if (b.heap && b.heap->backing().try_subview(b.offset, kTileBytes, &from)) tile.copy_from(from);
    else tile.fill(0);   // (nowhere: zeros)
  }
}

void VidMm::scatter_tiles(Allocation& a) {
  for (uint64_t i = 0; i < a.tiles_.count(); ++i) {
    const Allocation::TileBinding& b = a.tiles_[i];
    if (!b.heap) continue;
    ByteView from, to;
    if (a.is_image()) {
      for_tile_runs(a, i, a.cpu_, [&](ByteView image, uint64_t at, uint32_t texels) {
        if (b.heap->cpu_.try_subview(b.offset + at, uint64_t{texels} * tile_texel_bytes(a.desc_), &to)) heap_from_image(a.desc_, to, ConstByteView(image), texels);
      });
    } else if (a.cpu_.try_subview(i * kTileBytes, kTileBytes, &from) && b.heap->cpu_.try_subview(b.offset, kTileBytes, &to)) to.copy_from(ConstByteView(from));
    // (what lies in the heap's memory under placed allocations is theirs to see: their device objects are stale now)
    for (Allocation* other : b.heap->placed_) {
      other->device_stale_ = other->cpu_written_ = true;
      ++other->version_;
    }
  }
}

NtStatus VidMm::map_tiles(Allocation& reserved, uint32_t first, const SharedPointer<Allocation>& heap, uint64_t heap_offset, uint32_t count) {
  if (!reserved.reserved_ || count == 0 || first > reserved.tiles_.count() || count > reserved.tiles_.count() - first) return status::kInvalidParameter;
  if (heap && (!heap->is_heap() || &heap->manager() != this || heap_offset % kTileBytes || heap_offset > heap->desc().size || uint64_t{count} * kTileBytes > heap->desc().size - heap_offset))
    return status::kInvalidParameter;
  for (uint32_t i = 0; i < count; ++i) {
    reserved.tiles_[first + i].heap = heap;
    reserved.tiles_[first + i].offset = heap_offset + uint64_t{i} * kTileBytes;
  }
  reserved.device_stale_ = true;
  return status::kSuccess;
}

bool VidMm::upload(Allocation& a) {
  if (a.reserved_) gather_tiles(a);
  if (a.buffer_) {
    ByteView mapped = a.buffer_->map();
    const bool ok = mapped.copy_from(ConstByteView(a.cpu_));
    a.buffer_->unmap();
    if (!ok) return false;
    a.device_stale_ = false;
    return true;
  }
  if (!a.image_) return false;
  if (a.desc_.samples != 1) {
    a.device_stale_ = false;   // (a multisampled target has no pixels of the CPU's to upload)
    return true;
  }
  if (a.is_depth()) {
    a.device_stale_ = false;   // (nothing to upload: the contents of a depth buffer start undefined)
    return true;
  }
  const Format f = a.desc_.format;
  if (dxgk::is_wide(f)) {
    // (each layer of an array as Vec4s of its own; a plain image is one layer)
    ArrayView<graphics::Vec4> pixels;
    scratch_.clear();
    if (!scratch_.put_zeros(uint64_t{a.desc_.width} * a.desc_.height * sizeof(graphics::Vec4)) || !scratch_.writable_view().try_cast<graphics::Vec4>(&pixels)) return false;
    ByteReader in(ConstByteView(a.cpu_));
    const graphics::Format device_format = wide_format_of(f);
    const bool layered = a.desc_.kind != graphics::TextureKind::Texture2D;
    for (uint32_t layer = 0; layer < a.layers(); ++layer) {
      for (graphics::Vec4& c : pixels) c = graphics::decode_wide_pixel(device_format, in);
      if (!in.ok()) return false;
      const ArrayView<const graphics::Vec4> layer_pixels(pixels.data(), pixels.size());
      if (!(layered ? a.image_->write_layer_pixels_float(layer, layer_pixels) : a.image_->write_pixels_float(layer_pixels)) && a.cpu_written_) return false;
    }
    a.device_stale_ = false;
    return true;
  }
  if (dxgk::is_compressed(f)) {
    if (!upload_compressed(a)) return false;
    a.device_stale_ = false;
    return true;
  }
  const bool convert = !is_plain_bgra(f);
  if (a.desc_.kind != graphics::TextureKind::Texture2D) {
    const uint64_t layer_bytes = uint64_t{a.desc_.width} * a.desc_.height * 4;
    for (uint32_t layer = 0; layer < a.layers(); ++layer) {
      ArrayView<uint32_t> source;
      if (!as_words(a.cpu_.subview(layer_bytes * layer, layer_bytes), &source)) return false;
      ArrayView<const uint32_t> pixels(source.data(), source.size());
      if (convert) {
        scratch_.clear();
        if (!scratch_.put_zeros(layer_bytes)) return false;
        ArrayView<uint32_t> converted;
        if (!as_words(scratch_.writable_view(), &converted)) return false;
        for (uint64_t i = 0; i < converted.size(); ++i) converted[i] = to_logical(f, source[i]);
        pixels = ArrayView<const uint32_t>(converted.data(), converted.size());
      }
      if (!a.image_->write_layer_pixels(layer, pixels) && a.cpu_written_) return false;
    }
    a.device_stale_ = false;
    return true;
  }
  for (uint32_t level = 0; level < a.desc_.mip_levels; ++level) {
    uint64_t offset = 0, bytes = 0;
    if (!a.level_range(level, &offset, &bytes)) return false;
    ArrayView<uint32_t> source;
    if (!as_words(a.cpu_.subview(offset, bytes), &source)) return false;
    ArrayView<const uint32_t> pixels(source.data(), source.size());
    if (convert) {
      scratch_.clear();
      if (!scratch_.put_zeros(bytes)) return false;
      ArrayView<uint32_t> converted;
      if (!as_words(scratch_.writable_view(), &converted)) return false;
      for (uint64_t i = 0; i < converted.size(); ++i) converted[i] = to_logical(f, source[i]);
      pixels = ArrayView<const uint32_t>(converted.data(), converted.size());
    }
    if (!(level == 0 ? a.image_->write_pixels(pixels) : a.image_->write_mip_pixels(level, pixels))) {
      // A device that only draws into a colour target (virgl keeps targets and textures in opposite row order) takes no pixels for it; that costs nothing while the backing is still
      // the zeros it was made with, which is what a target holds until something draws into it.
      if (a.cpu_written_) return false;
    }
  }
  a.device_stale_ = false;
  return true;
}

// The blocks of a compressed texture to its device image: each level (or each layer of a volume, an array or a cube, which have one), as blocks where the device samples them
// (kFeatureCompressedFormats), else decoded into 32-bit pixels (the image was made of graphics::decoded_format).
bool VidMm::upload_compressed(Allocation& a) {
  const graphics::Format blocks_format = compressed_format_of(a.desc_.format);
  const bool native = (device_.info().features & graphics::kFeatureCompressedFormats) != 0;
  const bool layered = a.desc_.kind != graphics::TextureKind::Texture2D;
  const uint32_t parts = layered ? a.layers() : a.desc_.mip_levels;
  const uint64_t layer_bytes = image_level_bytes(a.desc_.format, a.desc_.width, a.desc_.height);
  for (uint32_t part = 0; part < parts; ++part) {
    const uint32_t level = layered ? 0 : part, layer = layered ? part : 0;
    uint64_t offset = 0, bytes = 0;
    if (!a.level_range(level, &offset, &bytes)) return false;
    if (layered) offset += layer_bytes * layer, bytes = layer_bytes;
    ConstByteView blocks;
    if (!ConstByteView(a.cpu_).try_subview(offset, bytes, &blocks)) return false;
    const uint32_t w = graphics::mip_extent(a.desc_.width, level), h = graphics::mip_extent(a.desc_.height, level);
    bool ok;
    if (native) {
      // (a volume's slices are the layers of the backing here; the device takes each slice as a layer of the transfer)
      ok = a.image_->write_compressed(level, layer, blocks);
    } else {
      scratch_.clear();
      ArrayView<uint32_t> pixels;
      if (!scratch_.put_zeros(uint64_t{w} * h * 4) || !as_words(scratch_.writable_view(), &pixels) || !graphics::decode_blocks(blocks_format, blocks, w, h, pixels)) return false;
      const ArrayView<const uint32_t> decoded(pixels.data(), pixels.size());
      ok = layered ? a.image_->write_layer_pixels(layer, decoded) : level == 0 ? a.image_->write_pixels(decoded) : a.image_->write_mip_pixels(level, decoded);
    }
    if (!ok && a.cpu_written_) return false;
  }
  return true;
}

bool VidMm::read_back(Allocation& a) {
  if (dxgk::is_compressed(a.desc_.format)) {   // (sampled only: the GPU never writes it)
    a.gpu_newer_ = false;
    return true;
  }
  if (!a.gpu_newer_) {
    if (a.reserved_) gather_tiles(a);   // (the heap's memory may have changed under it)
    return true;
  }
  if (a.buffer_) {
    // (a storage buffer a compute shader wrote: the device's contents come back into the backing)
    if (!a.buffer_->read_back()) return false;
    ConstByteView mapped = a.buffer_->map();
    if (!a.cpu_.copy_from(mapped.first(a.cpu_.size() < mapped.size() ? a.cpu_.size() : mapped.size()))) return false;
    if (a.reserved_) scatter_tiles(a);
    a.gpu_newer_ = false;
    return true;
  }
  if (!a.image_ || a.is_depth() || a.desc_.samples != 1) {   // (a multisampled image comes back as the resolved one)
    a.gpu_newer_ = false;
    return true;
  }
  const Format f = a.desc_.format;
  const bool layered = a.desc_.kind != graphics::TextureKind::Texture2D;   // (a layered render target or storage image: each layer)
  const uint64_t pixel_count = uint64_t{a.desc_.width} * a.desc_.height;
  if (dxgk::is_wide(f)) {
    ArrayView<graphics::Vec4> pixels;
    scratch_.clear();
    if (!scratch_.put_zeros(pixel_count * sizeof(graphics::Vec4)) || !scratch_.writable_view().try_cast<graphics::Vec4>(&pixels)) return false;
    const graphics::Format device_format = wide_format_of(f);
    const uint64_t layer_bytes = pixel_count * pixel_bytes(f);
    for (uint32_t layer = 0; layer < a.layers(); ++layer) {
      if (!(layered ? a.image_->read_layer_pixels_float(layer, pixels) : a.image_->read_pixels_float(pixels))) return false;
      ByteView to;
      if (!a.cpu_.try_subview(layer * layer_bytes, layer_bytes, &to)) return false;
      ByteWriter out(to);
      for (const graphics::Vec4& c : pixels) graphics::encode_wide_pixel(device_format, c, out);
      if (!out.ok()) return false;
    }
    a.gpu_newer_ = false;
    return true;
  }
  // (the 8-bit images: each layer of a layered one, level 0 of a plain one - and its other levels when shaders may have written them as a storage image)
  const uint32_t parts = layered ? a.layers() : (a.desc_.flags & kAllocStorageImage) ? a.desc_.mip_levels : 1;
  for (uint32_t part = 0; part < parts; ++part) {
    uint64_t offset = 0, bytes = 0;
    if (layered) {
      offset = part * pixel_count * 4, bytes = pixel_count * 4;
    } else if (!a.level_range(part, &offset, &bytes)) {
      return false;
    }
    ArrayView<uint32_t> backing;
    ByteView range;
    if (!a.cpu_.try_subview(offset, bytes, &range) || !as_words(range, &backing)) return false;
    auto read = [&](ArrayView<uint32_t> out) { return layered ? a.image_->read_layer_pixels(part, out) : part == 0 ? a.image_->read_pixels(out) : a.image_->read_mip_pixels(part, out); };
    if (is_plain_bgra(f)) {
      if (!read(backing)) return false;
    } else {
      scratch_.clear();
      if (!scratch_.put_zeros(backing.size() * 4)) return false;
      ArrayView<uint32_t> logical;
      if (!as_words(scratch_.writable_view(), &logical)) return false;
      if (!read(logical)) return false;
      for (uint64_t i = 0; i < backing.size(); ++i) backing[i] = from_logical(f, logical[i]);
    }
  }
  a.gpu_newer_ = false;
  return true;
}

namespace {
// Whether two placed allocations of one heap share bytes.
bool overlaps(const Allocation& a, const Allocation& b) {
  const uint64_t a_end = a.heap_offset() + a.backing().size(), b_end = b.heap_offset() + b.backing().size();
  return a.heap_offset() < b_end && b.heap_offset() < a_end;
}
}  // namespace

void VidMm::note_cpu_write(Allocation& a) {
  a.device_stale_ = true;
  a.cpu_written_ = true;
  ++a.version_;
  if (a.reserved_) scatter_tiles(a);
  if (a.heap_)
    for (Allocation* other : a.heap_->placed_)
      if (other != &a && overlaps(a, *other)) {
        other->device_stale_ = other->cpu_written_ = true;   // (its device object is out of date, and what it holds is now written by someone)
        ++other->version_;
      }
}
void VidMm::note_gpu_write(Allocation& a) {
  a.gpu_newer_ = true;
  a.last_use_ = ++clock_;
  ++a.version_;
}

// --- Mirrors --------------------------------------------------------------------------------------------------------------------------------------------------------

namespace {
// The bytes of one channel of a format a mirror may have or show (0: none): the 8-bit four-channel formats and the 16-bit ones, normalised or integer.
uint32_t mirror_channel_bytes(Format f) {
  switch (f) {
    case Format::A8B8G8R8: case Format::X8B8G8R8: case Format::A8B8G8R8Uint: return 1;
    case Format::A16B16G16R16: case Format::A16B16G16R16Uint: return 2;
    default: return 0;
  }
}
// The channel value of "one" in an integer image (the alpha Direct3D gives a view whose format has none) written into a pixel's alpha.
void set_integer_alpha(ByteView pixel, uint32_t channel) {
  ByteWriter w(pixel.subview(3 * channel, channel));
  if (channel == 1) w.u8(1);
  else w.le16(1);
}
}  // namespace

NtStatus VidMm::set_mirror(Allocation& mirror, ArrayView<const SharedPointer<Allocation>> sources, uint32_t mode, uint32_t channels) {
  if (mode & ~(kMirrorInteger | kMirrorWriteBack) || channels > 0xF) return status::kInvalidParameter;
  if (sources.empty()) {
    mirror.mirror_sources_.clear();
    mirror.mirror_mode_ = mirror.mirror_channels_ = 0;
    return status::kSuccess;
  }
  const AllocationDesc& m = mirror.desc_;
  const uint32_t channel = mirror_channel_bytes(m.format);
  const bool array = m.kind == graphics::TextureKind::Texture2DArray;
  if (!mirror.is_image() || mirror.is_mirror() || mirror.placed() || mirror.reserved_ || !(m.flags & kAllocTexture) || m.mip_levels != 1 || m.samples != 1 || channel == 0 ||
      (!array && m.kind != graphics::TextureKind::Texture2D) || sources.size() != (array ? m.layers : 1u))
    return status::kInvalidParameter;
  // (a mirror that writes back is a render target or a storage image - an array of them gives each layer to its source; one channel at least goes back)
  if ((mode & kMirrorWriteBack) && (!(m.flags & (kAllocRenderTarget | kAllocStorageImage)) || channels == 0)) return status::kInvalidParameter;
  for (const SharedPointer<Allocation>& source : sources) {
    if (!source || &source->manager() != this || source.get() == &mirror || !source->is_image() || source->is_mirror()) return status::kInvalidParameter;
    const AllocationDesc& d = source->desc_;
    if (d.kind != graphics::TextureKind::Texture2D || d.mip_levels != 1 || d.samples != 1 || d.width != m.width || d.height != m.height || mirror_channel_bytes(d.format) != channel)
      return status::kInvalidParameter;
  }
  List<Allocation::MirrorSource> made(allocator_);
  for (const SharedPointer<Allocation>& source : sources) {
    Allocation::MirrorSource ms;
    ms.source = source;
    if (!made.push_back(util::move(ms))) return status::kNoMemory;
  }
  mirror.mirror_sources_.clear();
  for (Allocation::MirrorSource& ms : made.view())
    if (!mirror.mirror_sources_.push_back(util::move(ms))) {
      mirror.mirror_sources_.clear();
      return status::kNoMemory;
    }
  mirror.mirror_mode_ = mode;
  mirror.mirror_channels_ = channels;
  return status::kSuccess;
}

bool VidMm::mirror_needs_read_back(const Allocation& mirror) const {
  for (const Allocation::MirrorSource& ms : mirror.mirror_sources_.view())
    if (ms.seen != ms.source->version_ && ms.source->gpu_newer_) return true;
  return false;
}

bool VidMm::refresh_mirror(Allocation& mirror, bool may_read_back, bool* stale) {
  if (stale) *stale = false;
  const uint32_t channel = mirror_channel_bytes(mirror.desc_.format);
  const uint64_t pixel = 4ull * channel, layer_bytes = uint64_t{mirror.desc_.width} * mirror.desc_.height * pixel;
  bool copied = false;
  for (uint64_t layer = 0; layer < mirror.mirror_sources_.count(); ++layer) {
    Allocation::MirrorSource& ms = mirror.mirror_sources_[layer];
    Allocation& source = *ms.source;
    if (ms.seen == source.version_) continue;
    if (source.gpu_newer_) {
      if (!may_read_back) {
        if (stale) *stale = true;
        continue;
      }
      if (!read_back(source)) return false;
    }
    ByteView to;
    ConstByteView from;
    if (!mirror.cpu_.try_subview(layer * layer_bytes, layer_bytes, &to) || !ConstByteView(source.cpu_).try_subview(0, layer_bytes, &from) || !to.copy_from(from)) return false;
    if (mirror.mirror_mode_ & kMirrorInteger)
      for (uint64_t at = 0; at < layer_bytes; at += pixel) set_integer_alpha(to.subview(at, pixel), channel);
    ms.seen = source.version_;
    copied = true;
  }
  if (copied) note_cpu_write(mirror);
  return true;
}

bool VidMm::write_back_mirror(Allocation& mirror) {
  if (!mirror.writes_back() || mirror.mirror_sources_.count() == 0 || !read_back(mirror)) return false;
  const uint32_t channel = mirror_channel_bytes(mirror.desc_.format);
  const uint64_t pixel = 4ull * channel, bytes = uint64_t{mirror.desc_.width} * mirror.desc_.height * pixel;
  // (each layer to its source: a mirror of one source has one)
  for (uint64_t layer = 0; layer < mirror.mirror_sources_.count(); ++layer) {
    Allocation& source = *mirror.mirror_sources_[layer].source;
    if (!read_back(source)) return false;
    ByteView to;
    ConstByteView from;
    if (!source.cpu_.try_subview(0, bytes, &to) || !ConstByteView(mirror.cpu_).try_subview(layer * bytes, bytes, &from)) return false;
    for (uint64_t at = 0; at < bytes; at += pixel)
      for (uint32_t c = 0; c < 4; ++c)
        if ((mirror.mirror_channels_ >> c & 1) && !to.subview(at + c * channel, channel).copy_from(from.subview(at + c * channel, channel))) return false;
    note_cpu_write(source);
    mirror.mirror_sources_[layer].seen = source.version_;   // (the mirror holds what it gave)
  }
  return true;
}

bool VidMm::switch_alias(Allocation& outgoing, Allocation& incoming) {
  if (!outgoing.heap_ || outgoing.heap_ != incoming.heap_) return false;
  if (outgoing.gpu_newer_ && !read_back(outgoing)) return false;
  // The memory holds the outgoing allocation's contents now; every allocation that shares any of it has a device object that does not.
  for (Allocation* other : outgoing.heap_->placed_)
    if (other != &outgoing && overlaps(outgoing, *other)) {
      if (other->gpu_newer_ && !read_back(*other)) return false;   // (what the GPU wrote there before: kept where it does not overlap)
      other->device_stale_ = other->cpu_written_ = true;
      ++other->version_;
    }
  incoming.device_stale_ = incoming.cpu_written_ = true;
  ++incoming.version_;
  return true;
}

bool VidMm::evict(Allocation& a) {
  if (a.pin_count_) return false;
  if (!a.resident()) return true;
  if (a.gpu_newer_) read_back(a);
  release_device_object(a);
  a.device_stale_ = true;
  a.gpu_newer_ = false;
  return true;
}

bool VidMm::evict_for(uint64_t needed, const Allocation* keep) {
  if (needed > budget_) return false;
  while (resident_bytes_ > budget_ - needed) {
    Allocation* victim = nullptr;
    for (uint64_t i = 0; i < resident_.count(); ++i) {
      Allocation* c = resident_[i];
      if (c == keep || c->pin_count_) continue;
      if (!victim || c->last_use_ < victim->last_use_) victim = c;
    }
    if (!victim) return false;
    evict(*victim);
  }
  return true;
}

void VidMm::trim() {
  // (evicting changes the list: start over after each)
  for (;;) {
    Allocation* victim = nullptr;
    for (uint64_t i = 0; i < resident_.count(); ++i)
      if (!resident_[i]->pin_count_) {
        victim = resident_[i];
        break;
      }
    if (!victim) return;
    evict(*victim);
  }
}

NtStatus VidMm::make_resident(Allocation& a) {
  a.last_use_ = ++clock_;
  if (!a.can_be_resident()) return status::kInvalidParameter;
  if (a.resident()) {
    if (a.device_stale_ && !upload(a)) return status::kInvalidDeviceState;
    return status::kSuccess;
  }
  if (!evict_for(a.device_bytes_, &a)) return status::kGraphicsNoVideoMemory;
  if (!create_device_object(a)) {
    // The device itself is out of room: give back everything that is not in use and try once more.
    trim();
    if (!create_device_object(a)) return status::kGraphicsNoVideoMemory;
  }
  resident_bytes_ += a.device_bytes_;
  if (!resident_.push_back(&a)) {
    release_device_object(a);
    return status::kNoMemory;
  }
  a.device_stale_ = true;
  if (!upload(a)) {
    evict(a);
    return status::kInvalidDeviceState;
  }
  return status::kSuccess;
}

}  // namespace dxgk
