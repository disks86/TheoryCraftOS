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

// CPU access to VRAM objects (nouveau's nvkm/subdev/instmem/nv50.c), two ways:
//
//  - PraminWindow: BAR0 0x700000 shows 1 MiB of VRAM; 0x001700 says which (the address >> 16, in 1 MiB steps). Works from Tesla to
//    Turing with nothing set up, so it is what page tables and the BAR instance blocks are written through. nouveau's "slow path".
//  - NvInstmem: once BAR2 is up (Fermi+), VRAM is reached through a few 64 KiB slots of BAR2 that are remapped on demand (a small
//    software TLB over the BAR2 VMM: nouveau maps each instance object into BAR2 instead; slots keep the BAR2 space needed small and
//    bounded). Falls back to PRAMIN while BAR2 is not available or a remap fails.
//
// Both move a window that every core shares, so each access holds a lock from moving the window to touching the word.
#pragma once
#include <stdint.h>
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvBar.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvMemory.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvRegisters.hpp"

namespace hardware {
namespace nv {

class PraminWindow final : public INvVramAccess {
 public:
  // ---- Registers (confident: nv50_instmem_rd32_slow / wr32_slow) ----
  static constexpr uint32_t kWindowBase = 0x001700;     // VRAM address >> 16 of the window's start (1 MiB aligned)
  static constexpr uint32_t kWindow = 0x700000;         // the window in BAR0
  static constexpr uint64_t kWindowBytes = 1u << 20;

  PraminWindow(INvRegisters& regs, uint64_t vram_bytes, INvLock& lock) : regs_(regs), vram_bytes_(vram_bytes), lock_(lock) {}

  [[nodiscard]] bool read32(uint64_t vram_address, uint32_t* value) override;
  [[nodiscard]] bool write32(uint64_t vram_address, uint32_t value) override;
  [[nodiscard]] bool fill32(uint64_t vram_address, uint64_t bytes, uint32_t value) override;
  // Someone else (the VBIOS, a resume) may have moved the window: program it again on the next access.
  void forget();
  uint32_t moves();

 private:
  bool place(uint64_t vram_address);   // lock held
  bool in_range(uint64_t vram_address, uint64_t bytes) const {
    return vram_address % 4 == 0 && bytes % 4 == 0 && vram_address <= vram_bytes_ && bytes <= vram_bytes_ - vram_address;
  }
  INvRegisters& regs_;
  uint64_t vram_bytes_;
  INvLock& lock_;
  uint64_t base_ = ~uint64_t{0};
  uint32_t moves_ = 0;
};

class NvInstmem final : public INvVramAccess {
 public:
  static constexpr uint32_t kMaxSlots = 16;
  static constexpr uint64_t kSlotBytes = 64 * 1024;

  NvInstmem(PraminWindow& pramin, uint64_t vram_bytes, INvLock& lock) : pramin_(pramin), vram_bytes_(vram_bytes), lock_(lock) {}

  // Starts using `slots` (1..16) slots of BAR2, at VA taken from `bar`'s BAR2 VMM; `aperture` is the CPU's mapping of PCI BAR2.
  [[nodiscard]] bool enable_bar2(NvBar& bar, INvAperture& aperture, uint32_t slots);
  void disable_bar2();

  [[nodiscard]] bool read32(uint64_t vram_address, uint32_t* value) override;
  [[nodiscard]] bool write32(uint64_t vram_address, uint32_t value) override;
  [[nodiscard]] bool flush_writes() override;

  bool bar2_active();
  uint32_t remaps();

 private:
  struct Slot {
    uint64_t chunk = ~uint64_t{0};   // VRAM address of the 64 KiB it shows
  };
  // The BAR2 offset that shows `vram_address` (remapping a slot if needed); empty: use PRAMIN. Lock held.
  Optional<uint64_t> through_bar2(uint64_t vram_address);

  PraminWindow& pramin_;
  uint64_t vram_bytes_;
  INvLock& lock_;
  NvBar* bar_ = nullptr;
  INvAperture* aperture_ = nullptr;
  uint64_t va_base_ = 0;
  Slot slots_[kMaxSlots];
  uint32_t slot_count_ = 0;
  uint32_t next_victim_ = 0;
  uint32_t remaps_ = 0;
};

}  // namespace nv
}  // namespace hardware
