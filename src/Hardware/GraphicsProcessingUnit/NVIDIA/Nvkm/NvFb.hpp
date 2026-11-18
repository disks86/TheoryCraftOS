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

// The frame buffer (nouveau's nvkm/subdev/fb and fb/ram*.c): how much VRAM the board has and of which kind, which parts of it the
// firmware keeps (the VGA area at the bottom, the VBIOS image and display workspace at the top), and the allocator that hands out the
// rest. NvVramMemory is the INvMemory for MemoryTarget::Vram on top of it; the CPU reaches the words through an INvVramAccess (the
// PRAMIN window or BAR2, Nvkm/NvInstmem.hpp).
#pragma once
#include <stdint.h>
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvChip.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvMemory.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvRegisters.hpp"
#include "Kernel/RawSpinLock.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/Optional.hpp"

namespace hardware {
namespace nv {

// The kernel's INvLock (see NvMemory.hpp for why the memory code takes an interface).
class RawNvLock final : public INvLock {
 public:
  void lock() override { lock_.lock(); }
  void unlock() override { lock_.unlock(); }

 private:
  kernel::RawSpinLock lock_;
};

enum class RamType : uint8_t { Unknown, Ddr1, Ddr2, Ddr3, Gddr3, Gddr4, Gddr5, Gddr5x, Gddr6, Gddr6x, Hbm2 };
const char* ram_type_name(RamType type);

enum class FbError : uint8_t {
  None,
  Unsupported,       // a family this code does not size (pre-Tesla, unknown)
  NeedsGsp,          // Ampere and later: the size is reported, but the memory is managed by the GSP firmware
  NoMemory,          // the registers say there is no VRAM
  Garbage,           // all ones, out of range, more partitions than a chip has: a powered-down or misidentified GPU
};

struct VramInfo {
  uint64_t bytes = 0;               // all of the VRAM
  RamType type = RamType::Unknown;
  bool type_guessed = false;        // from the family (no BIOS hint, no strap register)
  uint32_t partitions = 0;          // memory partitions (FBP/FBPA) that are enabled
  uint64_t partition_bytes = 0;     // the smallest enabled partition
  bool uneven = false;              // partitions of different sizes (nouveau's "mixed" configuration)
  uint64_t reserved_head = 0;       // bytes at the bottom the firmware keeps (VGA memory)
  uint64_t reserved_tail = 0;       // bytes at the top the firmware keeps (VBIOS image, display/VGA workspace)
  uint64_t usable_start() const { return reserved_head; }
  uint64_t usable_bytes() const { return bytes > reserved_head + reserved_tail ? bytes - reserved_head - reserved_tail : 0; }
};

struct VramProbe {
  FbError error = FbError::Unsupported;
  VramInfo info;
  bool ok() const { return error == FbError::None; }
};

// Reads the VRAM size, type and reserved regions for `chip` (registers in NvFb.cpp's table). `bios_type` is the memory type the VBIOS
// strap/ramcfg gives (nouveau's nvkm_fb_bios_memtype), Unknown when it was not read.
[[nodiscard]] VramProbe probe_vram(INvRegisters& regs, const Chip& chip, RamType bios_type = RamType::Unknown);

// The frame buffer's MMU setup that has to happen before the first page table is used (gf100_fb_init / gm200_fb_init): the big page
// size (Fermi..Maxwell: 16 or 17; Pascal on says it per page directory) and the system-memory flush page (`sysmem_flush_page`, a bus
// address of a 4 KiB page the MMU reads on a flush, 0 to leave it). False for a family this does not set up.
[[nodiscard]] bool fb_init_mmu(INvRegisters& regs, const Chip& chip, uint32_t big_page_shift, uint64_t sysmem_flush_page);

// First-fit allocator of address ranges with a fixed number of records (no heap: it is a member of the object that owns it). Used for
// VRAM (NvFb) and for the GPU virtual address spaces (NvVmm). Not locked: the owner serializes. Frees coalesce with their neighbours,
// and every allocation is remembered, so releasing a range that is not allocated (twice, or an address never handed out) is detected
// instead of corrupting the free list.
template <uint32_t Capacity>
class RangeAllocator {
 public:
  struct Range {
    uint64_t start = 0;
    uint64_t bytes = 0;
    uint64_t end() const { return start + bytes; }
  };

  // Everything forgotten; [start, start + bytes) free.
  [[nodiscard]] bool reset(uint64_t start, uint64_t bytes) {
    free_count_ = used_count_ = 0;
    if (bytes == 0 || bytes > ~uint64_t{0} - start) return false;
    free_view()[0] = Range{start, bytes};
    free_count_ = 1;
    return true;
  }

  // Takes [start, start + bytes) out of the free space for good (a region the firmware uses); false unless all of it is free.
  [[nodiscard]] bool reserve(uint64_t start, uint64_t bytes) {
    if (bytes == 0 || bytes > ~uint64_t{0} - start) return false;
    for (uint32_t i = 0; i < free_count_; ++i) {
      const Range f = free_view()[i];
      if (start < f.start || start - f.start > f.bytes || f.bytes - (start - f.start) < bytes) continue;
      return carve(i, start, bytes);
    }
    return false;
  }

  // `bytes` (non-zero) at an address aligned to `alignment` (a power of two); empty if no free range fits or the tables are full.
  [[nodiscard]] Optional<uint64_t> allocate(uint64_t bytes, uint64_t alignment) {
    if (bytes == 0 || alignment == 0 || (alignment & (alignment - 1)) != 0) return {};
    if (used_count_ >= Capacity) return {};
    for (uint32_t i = 0; i < free_count_; ++i) {
      const Range f = free_view()[i];
      if (f.start > ~uint64_t{0} - (alignment - 1)) continue;
      const uint64_t aligned = (f.start + alignment - 1) & ~(alignment - 1);
      const uint64_t pad = aligned - f.start;
      if (pad > f.bytes || f.bytes - pad < bytes) continue;
      if (!carve(i, aligned, bytes)) return {};
      insert_used(Range{aligned, bytes});
      return aligned;
    }
    return {};
  }

  // Gives back the range that starts at `start`; false (and nothing changes) if no allocation starts there.
  [[nodiscard]] bool release(uint64_t start, uint64_t* bytes_out = nullptr) {
    const Optional<uint32_t> at = find_used(start);
    if (!at) return false;
    const Range r = used_view()[*at];
    if (!insert_free(r)) return false;  // the free table is full: keep it allocated rather than lose track of it
    for (uint32_t i = *at; i + 1 < used_count_; ++i) used_view()[i] = used_view()[i + 1];
    --used_count_;
    if (bytes_out) *bytes_out = r.bytes;
    return true;
  }

  // The allocation that contains `address`, if any.
  Optional<Range> allocation_containing(uint64_t address) const {
    uint32_t lo = 0, hi = used_count_;
    while (lo < hi) {
      const uint32_t mid = (lo + hi) / 2;
      if (used_cview()[mid].start <= address) lo = mid + 1; else hi = mid;
    }
    if (lo == 0) return {};
    const Range r = used_cview()[lo - 1];
    if (address - r.start < r.bytes) return r;
    return {};
  }

  uint64_t free_bytes() const {
    uint64_t total = 0;
    for (uint32_t i = 0; i < free_count_; ++i) total += free_cview()[i].bytes;
    return total;
  }
  uint64_t largest_free() const {
    uint64_t best = 0;
    for (uint32_t i = 0; i < free_count_; ++i) best = free_cview()[i].bytes > best ? free_cview()[i].bytes : best;
    return best;
  }
  uint32_t allocation_count() const { return used_count_; }
  uint32_t free_range_count() const { return free_count_; }

 private:
  static constexpr uint32_t kFreeCapacity = Capacity + 8;  // free ranges lie between allocations and reservations

  ArrayView<Range> free_view() { return ArrayView<Range>(free_); }
  ArrayView<Range> used_view() { return ArrayView<Range>(used_); }
  ArrayView<const Range> free_cview() const { return ArrayView<const Range>(free_); }
  ArrayView<const Range> used_cview() const { return ArrayView<const Range>(used_); }

  // Removes [start, start + bytes) from free range `index` (which contains it), leaving up to two pieces.
  bool carve(uint32_t index, uint64_t start, uint64_t bytes) {
    const Range f = free_view()[index];
    const Range before{f.start, start - f.start};
    const Range after{start + bytes, f.end() - (start + bytes)};
    if (before.bytes && after.bytes) {
      if (free_count_ >= kFreeCapacity) return false;
      for (uint32_t i = free_count_; i > index + 1; --i) free_view()[i] = free_view()[i - 1];
      ++free_count_;
      free_view()[index] = before;
      free_view()[index + 1] = after;
    } else if (before.bytes || after.bytes) {
      free_view()[index] = before.bytes ? before : after;
    } else {
      for (uint32_t i = index; i + 1 < free_count_; ++i) free_view()[i] = free_view()[i + 1];
      --free_count_;
    }
    return true;
  }

  bool insert_free(Range r) {
    uint32_t pos = 0;
    while (pos < free_count_ && free_view()[pos].start < r.start) ++pos;
    const bool join_prev = pos > 0 && free_view()[pos - 1].end() == r.start;
    const bool join_next = pos < free_count_ && r.end() == free_view()[pos].start;
    if (join_prev && join_next) {
      free_view()[pos - 1].bytes += r.bytes + free_view()[pos].bytes;
      for (uint32_t i = pos; i + 1 < free_count_; ++i) free_view()[i] = free_view()[i + 1];
      --free_count_;
    } else if (join_prev) {
      free_view()[pos - 1].bytes += r.bytes;
    } else if (join_next) {
      free_view()[pos].start = r.start;
      free_view()[pos].bytes += r.bytes;
    } else {
      if (free_count_ >= kFreeCapacity) return false;
      for (uint32_t i = free_count_; i > pos; --i) free_view()[i] = free_view()[i - 1];
      free_view()[pos] = r;
      ++free_count_;
    }
    return true;
  }

  void insert_used(Range r) {
    uint32_t pos = used_count_;
    while (pos > 0 && used_view()[pos - 1].start > r.start) {
      used_view()[pos] = used_view()[pos - 1];
      --pos;
    }
    used_view()[pos] = r;
    ++used_count_;
  }

  Optional<uint32_t> find_used(uint64_t start) const {
    uint32_t lo = 0, hi = used_count_;
    while (lo < hi) {
      const uint32_t mid = (lo + hi) / 2;
      if (used_cview()[mid].start < start) lo = mid + 1; else hi = mid;
    }
    if (lo < used_count_ && used_cview()[lo].start == start) return lo;
    return {};
  }

  Range free_[kFreeCapacity];
  Range used_[Capacity];
  uint32_t free_count_ = 0;
  uint32_t used_count_ = 0;
};

// The VRAM heap: the usable part of VRAM (between the reserved head and tail), handed out in 256-byte-aligned pieces. Every call takes
// the lock, so several cores may allocate and release at once.
class NvFb {
 public:
  static constexpr uint32_t kMaxAllocations = 4096;
  static constexpr uint64_t kMinAlignment = 256;

  explicit NvFb(INvLock& lock) : lock_(lock) {}
  NvFb(const NvFb&) = delete;
  NvFb& operator=(const NvFb&) = delete;

  // Starts the heap over `info`'s usable range; false if there is none.
  [[nodiscard]] bool init(const VramInfo& info);
  // A VRAM address (a multiple of `alignment`, raised to at least 256); empty if there is no room.
  [[nodiscard]] Optional<uint64_t> allocate(uint64_t bytes, uint64_t alignment);
  // False for an address that is not the start of a live allocation (a double release): counted, nothing changes.
  [[nodiscard]] bool release(uint64_t address);
  // True if [address, address + bytes) lies inside one live allocation.
  bool owns(uint64_t address, uint64_t bytes);

  const VramInfo& info() const { return info_; }
  uint64_t free_bytes();
  uint32_t allocation_count();
  uint64_t bad_releases() { NvLockGuard guard(lock_); return bad_releases_; }

 private:
  INvLock& lock_;
  VramInfo info_;
  RangeAllocator<kMaxAllocations> heap_;
  uint64_t bad_releases_ = 0;
};

// The INvMemory the rest of the driver takes: VRAM objects from NvFb (rounded to 256 bytes, or to whole 4 KiB pages when the alignment
// asked for is at least a page, so they can be mapped whole), their words through `access`; system-memory targets go to
// `system` (the kernel's DMA allocator, or a fake), or fail when there is none. VRAM objects have no `cpu` view (see INvMemory::read32).
class NvVramMemory final : public INvMemory {
 public:
  NvVramMemory(NvFb& fb, INvVramAccess& access, INvMemory* system = nullptr) : fb_(fb), access_(access), system_(system) {}

  [[nodiscard]] GpuMemory allocate(MemoryTarget target, uint64_t bytes, uint64_t alignment) override;
  void release(GpuMemory& memory) override;
  void flush(const GpuMemory& memory) override;
  uint64_t vram_bytes() const override { return fb_.info().bytes; }
  [[nodiscard]] bool read32(const GpuMemory& memory, uint64_t offset, uint32_t* value) override;
  [[nodiscard]] bool write32(const GpuMemory& memory, uint64_t offset, uint32_t value) override;

  INvVramAccess& access() { return access_; }

 private:
  NvFb& fb_;
  INvVramAccess& access_;
  INvMemory* system_;
};

}  // namespace nv
}  // namespace hardware
