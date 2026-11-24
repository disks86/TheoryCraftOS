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
#include "Utility/RangeAllocator.hpp"

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
// probe_vram for a driver of the display alone (NvidiaSecondaryGpu): the same, except that GA102..GA107, whose display nouveau drives without
// GSP, get their size (0x1183a4, as ga102_ram_new) and reserved regions with FbError::None instead of NeedsGsp. The VRAM is then this
// driver's only for the display's objects; nothing else of the chip is run.
[[nodiscard]] VramProbe probe_vram_display(INvRegisters& regs, const Chip& chip, RamType bios_type = RamType::Unknown);

// The frame buffer's MMU setup that has to happen before the first page table is used (gf100_fb_init / gm200_fb_init): the big page
// size (Fermi..Maxwell: 16 or 17; Pascal on says it per page directory) and the system-memory flush page (`sysmem_flush_page`, a bus
// address of a 4 KiB page the MMU reads on a flush, 0 to leave it). False for a family this does not set up.
[[nodiscard]] bool fb_init_mmu(INvRegisters& regs, const Chip& chip, uint32_t big_page_shift, uint64_t sysmem_flush_page);

using ::RangeAllocator;   // (Utility/RangeAllocator.hpp)

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
