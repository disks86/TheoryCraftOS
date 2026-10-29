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
#include "Graphics/Graphics3D/Mipmaps.hpp"
#include "Utility/AllocatedView.hpp"
#include "Utility/ByteStream.hpp"

namespace dxgk {

namespace {

constexpr uint64_t kMaxQueryResultBytes = 1 << 20;
constexpr uint32_t kKnownFlags = kAllocVertexBuffer | kAllocIndexBuffer | kAllocTexture | kAllocRenderTarget | kAllocDepthStencil | kAllocPresent | kAllocStaging | kAllocShared | kAllocQueryResult;

// The bytes of all levels of a w x h image of `pixel`-byte pixels; false if the description's levels are not possible. (w and h are at most kMaxTextureDimension, so nothing overflows.)
bool image_bytes(uint32_t w, uint32_t h, uint32_t levels, uint32_t pixel, uint64_t* total) {
  uint64_t sum = 0;
  for (uint32_t l = 0; l < levels; ++l) sum += uint64_t{graphics::mip_extent(w, l)} * graphics::mip_extent(h, l) * pixel;
  *total = sum;
  return true;
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
    *bytes = uint64_t{desc_.width} * desc_.height * pixel_bytes(desc_.format) * layers();
    return true;
  }
  uint64_t start = 0;
  const uint32_t pixel = pixel_bytes(desc_.format);
  for (uint32_t l = 0; l < level; ++l) start += uint64_t{graphics::mip_extent(desc_.width, l)} * graphics::mip_extent(desc_.height, l) * pixel;
  *offset = start;
  *bytes = uint64_t{graphics::mip_extent(desc_.width, level)} * graphics::mip_extent(desc_.height, level) * pixel;
  return true;
}

Allocation::Allocation(VidMm& manager, allocators::IAllocator& allocator, const AllocationDesc& desc, uint64_t id)
    : manager_(manager), allocator_(allocator), desc_(desc), id_(id) {}

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

NtStatus VidMm::create(const AllocationDesc& desc, SharedPointer<Allocation>* out) {
  if (desc.flags == 0 || (desc.flags & ~kKnownFlags)) return status::kInvalidParameter;
  const bool buffer = (desc.flags & (kAllocVertexBuffer | kAllocIndexBuffer | kAllocQueryResult)) != 0;
  uint64_t cpu_bytes = 0, device_bytes = 0;
  if (desc.flags & kAllocQueryResult) {
    // (results are written into system memory by the CPU: no device object)
    if (desc.flags & ~(kAllocQueryResult | kAllocShared)) return status::kInvalidParameter;
    if (!(device_.info().features & graphics::kFeatureOcclusionQueries)) return status::kNotSupported;
    if (desc.size < 8 || desc.size > kMaxQueryResultBytes) return status::kInvalidParameter;
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
    if (!depth_flag && !dxgk::is_color(desc.format)) return status::kInvalidParameter;
    if (desc.flags & kAllocStaging) {
      if (desc.flags & ~(kAllocStaging | kAllocShared)) return status::kInvalidParameter;
    } else if (!(desc.flags & (kAllocTexture | kAllocRenderTarget | kAllocDepthStencil | kAllocPresent))) {
      return status::kInvalidParameter;
    }
    if (depth_flag && (desc.flags & ~(kAllocDepthStencil | kAllocShared))) return status::kInvalidParameter;
    if (dxgk::is_float(desc.format)) {
      if (desc.mip_levels != 1 || (desc.flags & kAllocPresent)) return status::kInvalidParameter;
      if (!(desc.flags & kAllocStaging) && !(device_.info().features & graphics::kFeatureFloatFormats)) return status::kNotSupported;
    }
    if (desc.mip_levels > 1 && (desc.flags & ~(kAllocTexture | kAllocShared))) return status::kInvalidParameter;
    uint64_t total = 0;
    image_bytes(desc.width, desc.height, desc.mip_levels, pixel_bytes(desc.format), &total);
    if (desc.kind != graphics::TextureKind::Texture2D) {
      // (a volume, array or cube: sampled only, one level, 8-bit colour)
      if ((desc.flags & ~(kAllocTexture | kAllocShared)) || !(desc.flags & kAllocTexture) || desc.mip_levels != 1 || depth_flag || dxgk::is_float(desc.format)) return status::kInvalidParameter;
      if (desc.layers == 0 || desc.layers > graphics::kMaxTextureLayers || (desc.kind == graphics::TextureKind::TextureCube && (desc.layers != 6 || desc.width != desc.height))) return status::kInvalidParameter;
      if (!(device_.info().features & graphics::kFeatureTextureKinds)) return status::kNotSupported;
      total *= desc.layers;
    } else if (desc.layers != 1) {
      return status::kInvalidParameter;
    }
    if (depth_flag) {
      device_bytes = uint64_t{desc.width} * desc.height * (desc.format == Format::D24S8 ? 5 : 4);
    } else {
      cpu_bytes = total;
      device_bytes = desc.flags & kAllocStaging ? 0 : total;
    }
  }
  if (cpu_bytes > system_limit_ || system_bytes_ > system_limit_ - cpu_bytes) return status::kGraphicsNoVideoMemory;

  SharedPointer<Allocation> a = SharedPointer<Allocation>::make(allocator_, *this, allocator_, desc, next_id_);
  if (!a) return status::kNoMemory;
  if (cpu_bytes) {
    a->cpu_ = allocate_view<uint8_t>(allocator_, cpu_bytes, 16);
    if (a->cpu_.empty()) return status::kNoMemory;
    a->cpu_.fill(0);
  }
  ++next_id_;
  a->device_bytes_ = device_bytes;
  system_bytes_ += cpu_bytes;
  *out = a;
  return status::kSuccess;
}

void VidMm::forget(Allocation& a) {
  if (a.resident()) release_device_object(a);
  system_bytes_ -= a.cpu_.size();
  release_view(allocator_, a.cpu_);
}

void VidMm::release_device_object(Allocation& a) {
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
    a.buffer_ = device_.create_buffer(d.size, usage);
    return a.buffer_ != nullptr;
  }
  graphics::ImageDesc id;
  id.extent = {d.width, d.height};
  id.mip_levels = d.mip_levels;
  id.kind = d.kind;
  id.layers = d.layers;
  if (a.is_depth()) {
    id.format = graphics::Format::D32Float;
    id.usage = graphics::kImageDepthAttachment;
    id.stencil = d.format == Format::D24S8;
    a.image_ = device_.create_image(id);
    return a.image_ != nullptr;
  }
  id.format = d.format == Format::A16B16G16R16F ? graphics::Format::R16G16B16A16Float
              : d.format == Format::A32B32G32R32F ? graphics::Format::R32G32B32A32Float
              : is_rgba_order(d.format) ? graphics::Format::R8G8B8A8Unorm : graphics::Format::B8G8R8A8Unorm;
  id.usage = 0;
  if (d.flags & kAllocRenderTarget) id.usage |= graphics::kImageColorAttachment;
  if (d.flags & kAllocPresent) id.usage |= graphics::kImagePresent;
  if (d.flags & kAllocTexture) id.usage |= graphics::kImageSampled;
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

bool VidMm::upload(Allocation& a) {
  if (a.buffer_) {
    ByteView mapped = a.buffer_->map();
    const bool ok = mapped.copy_from(ConstByteView(a.cpu_));
    a.buffer_->unmap();
    if (!ok) return false;
    a.device_stale_ = false;
    return true;
  }
  if (!a.image_) return false;
  if (a.is_depth()) {
    a.device_stale_ = false;   // (nothing to upload: the contents of a depth buffer start undefined)
    return true;
  }
  const Format f = a.desc_.format;
  if (dxgk::is_float(f)) {
    ArrayView<graphics::Vec4> pixels;
    scratch_.clear();
    if (!scratch_.put_zeros(uint64_t{a.desc_.width} * a.desc_.height * sizeof(graphics::Vec4)) || !scratch_.writable_view().try_cast<graphics::Vec4>(&pixels)) return false;
    ByteReader in(ConstByteView(a.cpu_));
    for (graphics::Vec4& c : pixels) {
      if (f == Format::A16B16G16R16F) {
        c = {graphics::half_to_float(static_cast<uint16_t>(in.le16())), graphics::half_to_float(static_cast<uint16_t>(in.le16())), graphics::half_to_float(static_cast<uint16_t>(in.le16())),
             graphics::half_to_float(static_cast<uint16_t>(in.le16()))};
      } else {
        c = {__builtin_bit_cast(float, in.le32()), __builtin_bit_cast(float, in.le32()), __builtin_bit_cast(float, in.le32()), __builtin_bit_cast(float, in.le32())};
      }
    }
    if (!in.ok()) return false;
    if (!a.image_->write_pixels_float(ArrayView<const graphics::Vec4>(pixels.data(), pixels.size())) && a.cpu_written_) return false;
    a.device_stale_ = false;
    return true;
  }
  const bool convert = f != Format::A8R8G8B8;
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

bool VidMm::read_back(Allocation& a) {
  if (!a.gpu_newer_) return true;
  if (!a.image_ || a.is_depth()) {
    a.gpu_newer_ = false;
    return true;
  }
  const Format f = a.desc_.format;
  if (dxgk::is_float(f)) {
    ArrayView<graphics::Vec4> pixels;
    scratch_.clear();
    if (!scratch_.put_zeros(uint64_t{a.desc_.width} * a.desc_.height * sizeof(graphics::Vec4)) || !scratch_.writable_view().try_cast<graphics::Vec4>(&pixels)) return false;
    if (!a.image_->read_pixels_float(pixels)) return false;
    ByteWriter out(a.cpu_.first(uint64_t{a.desc_.width} * a.desc_.height * pixel_bytes(f)));
    for (const graphics::Vec4& c : pixels) {
      if (f == Format::A16B16G16R16F) {
        out.le16(graphics::float_to_half(c.x)), out.le16(graphics::float_to_half(c.y)), out.le16(graphics::float_to_half(c.z)), out.le16(graphics::float_to_half(c.w));
      } else {
        out.le32(__builtin_bit_cast(uint32_t, c.x)), out.le32(__builtin_bit_cast(uint32_t, c.y)), out.le32(__builtin_bit_cast(uint32_t, c.z)), out.le32(__builtin_bit_cast(uint32_t, c.w));
      }
    }
    if (!out.ok()) return false;
    a.gpu_newer_ = false;
    return true;
  }
  ArrayView<uint32_t> backing;
  if (!as_words(a.cpu_.first(uint64_t{a.desc_.width} * a.desc_.height * 4), &backing)) return false;
  if (f == Format::A8R8G8B8) {
    if (!a.image_->read_pixels(backing)) return false;
  } else {
    scratch_.clear();
    if (!scratch_.put_zeros(backing.size() * 4)) return false;
    ArrayView<uint32_t> logical;
    if (!as_words(scratch_.writable_view(), &logical)) return false;
    if (!a.image_->read_pixels(logical)) return false;
    for (uint64_t i = 0; i < backing.size(); ++i) backing[i] = from_logical(f, logical[i]);
  }
  a.gpu_newer_ = false;
  return true;
}

void VidMm::note_cpu_write(Allocation& a) {
  a.device_stale_ = true;
  a.cpu_written_ = true;
}
void VidMm::note_gpu_write(Allocation& a) {
  a.gpu_newer_ = true;
  a.last_use_ = ++clock_;
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
