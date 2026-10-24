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


#pragma once
#include <stdint.h>
#include "Hardware/Mmio.hpp"
#include "Hardware/RegisterMapper.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {

// Moving bytes in and out of device memory through the checked window: every access is naturally aligned and as wide as the position allows (bytes up to the next
// eight-byte boundary, then eight bytes at a time, then the tail in bytes), which is what uncached video memory wants. The window itself checks the range.
inline void mmio_write_bytes(const MmioRegion& region, uint64_t offset, ConstByteView data) {
  uint64_t i = 0;
  const uint64_t n = data.size();
  while (i < n && ((region.address() + offset + i) & 7) != 0) { region.write8(offset + i, data[i]); ++i; }
  for (; n - i >= 8; i += 8) {
    uint64_t v = 0;
    for (uint32_t k = 0; k < 8; ++k) v |= uint64_t{data[i + k]} << (8 * k);
    region.write64(offset + i, v);
  }
  for (; i < n; ++i) region.write8(offset + i, data[i]);
}

inline void mmio_read_bytes(const MmioRegion& region, uint64_t offset, ByteView out) {
  uint64_t i = 0;
  const uint64_t n = out.size();
  while (i < n && ((region.address() + offset + i) & 7) != 0) { out[i] = region.read8(offset + i); ++i; }
  for (; n - i >= 8; i += 8) {
    const uint64_t v = region.read64(offset + i);
    for (uint32_t k = 0; k < 8; ++k) out[i + k] = static_cast<uint8_t>(v >> (8 * k));
  }
  for (; i < n; ++i) out[i] = region.read8(offset + i);
}

inline void mmio_fill(const MmioRegion& region, uint64_t offset, uint64_t length, uint8_t value) {
  uint64_t i = 0;
  uint64_t pattern = 0;
  for (uint32_t k = 0; k < 8; ++k) pattern |= uint64_t{value} << (8 * k);
  while (i < length && ((region.address() + offset + i) & 7) != 0) { region.write8(offset + i, value); ++i; }
  for (; length - i >= 8; i += 8) region.write64(offset + i, pattern);
  for (; i < length; ++i) region.write8(offset + i, value);
}

// The part of video memory a decode session may use, handed out in pages from the front (a bump allocator: the memory comes back all at once, when the session
// that took it is closed). The video engine's DMA goes to these addresses, so nothing outside [gpu_base, gpu_base + bytes) is ever handed out; each piece is mapped
// for the CPU through the mapper, which refuses memory the OS owns.
class AmdVcnWorkMemory {
 public:
  static constexpr uint64_t kPage = 4096;
  struct Piece {
    uint64_t gpu_address = 0;   // as the GPU addresses it
    MmioRegion cpu;             // as the CPU does
    uint64_t bytes = 0;
    bool valid() const { return bytes != 0; }
  };

  AmdVcnWorkMemory() = default;
  // `bytes` of video memory starting at GPU address `gpu_base` and CPU physical address `physical_base` (both page aligned; otherwise nothing is configured).
  bool configure(IRegisterMapper* mapper, uint64_t gpu_base, uint64_t physical_base, uint64_t bytes) {
    mapper_ = nullptr;
    used_ = permanent_ = 0;
    if (!mapper || bytes == 0 || (gpu_base | physical_base | bytes) % kPage != 0 || gpu_base > UINT64_MAX - bytes || physical_base > UINT64_MAX - bytes) return false;
    mapper_ = mapper;
    gpu_base_ = gpu_base;
    physical_base_ = physical_base;
    bytes_ = bytes;
    return true;
  }
  bool configured() const { return mapper_ != nullptr; }
  uint64_t free_bytes() const { return mapper_ ? bytes_ - used_ : 0; }
  // What a session can have in all once it has reset the memory: everything but the permanent part.
  uint64_t capacity_bytes() const { return mapper_ ? bytes_ - permanent_ : 0; }

  // A piece of at least `bytes` (rounded up to whole pages, and aligned to `alignment`, a power of two from a page up), mapped for the CPU. False when it does not fit
  // or the mapper refuses it; then nothing is taken.
  bool allocate(uint64_t bytes, uint64_t alignment, Piece* out) {
    if (!mapper_ || bytes == 0 || alignment < kPage || (alignment & (alignment - 1)) != 0 || bytes > bytes_) return false;
    const uint64_t rounded = (bytes + kPage - 1) / kPage * kPage;
    const uint64_t start = (used_ + alignment - 1) / alignment * alignment;
    if (start > bytes_ || rounded > bytes_ - start) return false;
    MmioRegion region;
    if (!mapper_->map_registers(physical_base_ + start, rounded, &region)) return false;
    out->gpu_address = gpu_base_ + start;
    out->cpu = region;
    out->bytes = rounded;
    used_ = start + rounded;
    return true;
  }
  // One session at a time owns the work memory: try_lease() is true for the first caller until release_lease().
  bool try_lease() {
    if (leased_) return false;
    leased_ = true;
    return true;
  }
  void release_lease() { leased_ = false; }
  // Marks everything allocated so far as permanent: reset() keeps it.
  void keep() { permanent_ = used_; }
  // Gives back everything taken since keep() (or since configure()). The pieces must not be used afterwards.
  void reset() { used_ = permanent_; }

 private:
  IRegisterMapper* mapper_ = nullptr;
  uint64_t gpu_base_ = 0, physical_base_ = 0, bytes_ = 0;
  uint64_t used_ = 0, permanent_ = 0;
  bool leased_ = false;
};

}  // namespace hardware
