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

// The BARs through which the CPU sees GPU memory (nouveau's nvkm/subdev/bar: gf100.c, gm107.c, tu102.c). From Fermi on, BAR1 (PCI
// resource 1, the "frame buffer" BAR) and BAR2 (PCI resource 3 in nouveau's numbering, the kernel's instance-memory BAR) are virtual:
// each has an instance block whose page directory says where an offset into the BAR lands. BAR1 carries USERD (the channels' GP_PUT,
// Kepler..Pascal) and mappings handed to user mode; BAR2 is how NvInstmem reaches VRAM objects quickly.
//
// Firmware note: before 0x001704 is written, BAR1 maps VRAM linearly, and the UEFI GOP frame buffer the console draws into is reached
// that way. init() keeps the first `keep_linear_bytes` of BAR1 mapped 1:1 to VRAM so that keeps working.
#pragma once
#include <stdint.h>
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvMmu.hpp"
#include "Hardware/Mmio.hpp"

namespace hardware {
namespace nv {

// A PCI BAR aperture, word at a time (MMIO: no plain memory view, see INvMemory::read32).
class INvAperture {
 public:
  virtual uint32_t read32(uint64_t offset) = 0;
  virtual void write32(uint64_t offset, uint32_t value) = 0;
  virtual uint64_t bytes() const = 0;

 protected:
  INvAperture() = default;
  ~INvAperture() = default;
};

class MmioAperture final : public INvAperture {
 public:
  explicit MmioAperture(const MmioRegion& region) : region_(region) {}
  uint32_t read32(uint64_t offset) override { return offset % 4 == 0 && offset + 4 <= region_.length() ? region_.read32(offset) : 0xFFFFFFFFu; }
  void write32(uint64_t offset, uint32_t value) override { if (offset % 4 == 0 && offset + 4 <= region_.length()) region_.write32(offset, value); }
  uint64_t bytes() const override { return region_.length(); }

 private:
  MmioRegion region_;
};

enum class BarStatus : uint8_t { Ok, Unsupported, NoMemory, BadSize, Timeout };

class NvBar {
 public:
  // `tables` must reach VRAM without BAR2 (the PRAMIN-backed NvVramMemory).
  NvBar(INvRegisters& regs, const Chip& chip, INvMemory& tables, NvTlb& tlb, INvLock& bar1_lock, INvLock& bar2_lock, NvTimeouts timeouts = {});
  ~NvBar();
  NvBar(const NvBar&) = delete;
  NvBar& operator=(const NvBar&) = delete;

  // Builds both BARs' instance blocks and page directories and points the GPU at them (BAR2 first, as nouveau does). The sizes are
  // the PCI BAR sizes (BAR1: resource 1, BAR2: resource 3). Fermi..Turing; Tesla's DMA-object BARs are not done (Unsupported).
  [[nodiscard]] BarStatus init(uint64_t bar1_bytes, uint64_t bar2_bytes, uint64_t keep_linear_bytes = 0);
  // Turns the virtual BARs off again (bit 31 of the instance registers); BAR1 goes back to the firmware's linear mapping.
  void fini();
  // g84_bar_flush: makes CPU writes through the BARs visible to the GPU. False on a timeout.
  [[nodiscard]] bool flush_writes();

  bool ready() const { return ready_; }
  NvVmm& bar1() { return bar1_; }
  NvVmm& bar2() { return bar2_; }
  const GpuMemory& bar1_instance() const { return bar1_inst_; }
  const GpuMemory& bar2_instance() const { return bar2_inst_; }

 private:
  bool poll_clear(uint32_t offset, uint32_t mask);
  INvRegisters& regs_;
  Chip chip_;
  INvMemory& tables_;
  NvTimeouts timeouts_;
  NvVmm bar1_, bar2_;
  GpuMemory bar1_inst_, bar2_inst_;
  bool ready_ = false;
};

}  // namespace nv
}  // namespace hardware
