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
#include "Hardware/GraphicsProcessingUnit/Broadcom/V3dPageTable.hpp"
#include "Hardware/GraphicsProcessingUnit/Broadcom/V3dRegisters.hpp"
#include "Hardware/Mmio.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {

// What the identification registers of a V3D say it is.
struct V3dIdentity {
  bool valid = false;
  uint32_t technology_version = 0;  // 4 for V3D 4.x
  uint32_t revision = 0;            // 2 for V3D 4.2 (the VideoCore VI of the BCM2711)
  uint32_t version() const { return technology_version * 10 + revision; }  // 42
  uint32_t cores = 0, slices = 0, qpus_per_slice = 0, texture_units = 0, semaphores = 0, vpm_kib = 0;
  bool with_mmu = false, with_tfu = false, with_l3_cache = false;
  uint32_t qpus() const { return slices * qpus_per_slice; }
};

enum class V3dJobResult : uint8_t {
  Done,
  TimedOut,   // the list did not finish in the attempts given
  Error,      // the core flagged an error or the MMU a violation (see V3dGpu::last_error())
  OutOfMemory // binner ran out of tile memory and no overflow block was given
};

// A block of GPU memory (a GPU address and its size), as the binner takes them.
struct V3dMemoryBlock {
  uint32_t address = 0;
  uint32_t bytes = 0;
};

// What the binner needs besides its control list: the tile allocation memory (the lists of primitives per tile) and the tile state array.
struct V3dBinJob {
  uint32_t start = 0, end = 0;   // the binner's control list [start, end)
  V3dMemoryBlock tile_alloc;      // CT0QMA / CT0QMS
  uint32_t tile_state = 0;        // CT0QTS (address; 0 = leave as it is)
};

// The V3D 4.2 graphics core: finds out what it is, turns the caches and the MMU on, and runs control lists - the binner's (CT0) and the renderer's (CT1). A control
// list is the command stream the core's front end reads (packets for state, tile setup, draws and stores); this class does not make one, it starts the one it is given
// at a GPU address and waits for the core to say it is done. It polls: interrupts stay masked.
// Bring-up and job start follow the Linux v3d driver; it has only been run against a pretend register file here.
class V3dGpu {
 public:
  // `hub` and `core` are the register windows (V3D registers are 32 bits wide; each at least 0x4000 bytes).
  V3dGpu(const MmioRegion& hub, const MmioRegion& core) : hub_(hub), core_(core) {}

  // Reads the identification registers without changing anything.
  static V3dIdentity identify(const MmioRegion& hub, const MmioRegion& core);

  // Checks that this is a V3D 4.x with an MMU, masks and clears interrupts, enables the L2 cache and clears the slice caches. False if it is something else.
  bool initialize();
  // Masks interrupts and turns the MMU off. Safe to call at any time.
  void shutdown();
  bool initialized() const { return initialized_; }
  const V3dIdentity& identity() const { return identity_; }

  // Points the MMU at a page table (at bus address `table_bus_address`, 4 KiB aligned) and turns it on. Pages the GPU touches that are not mapped read as the
  // "illegal" page (`illegal_page_bus_address`, a 4 KiB page nothing else uses) and raise the invalid-page interrupt. Call after initialize().
  bool enable_mmu(uint64_t table_bus_address, uint64_t illegal_page_bus_address);
  // Drops the MMU's cached translations after the page table changed.
  bool flush_mmu(uint32_t attempts = 100000);

  // Invalidates the L2 cache, the slice caches and the L2 texture cache (the core must be idle): done before each job so it sees what the CPU wrote.
  bool flush_caches(uint32_t attempts = 100000);

  // Runs the binner's control list and waits for the flush-done interrupt bit, polling at most `attempts` times. Each time the binner runs out of tile memory it is
  // given the next of `overflow` (the Linux driver hands out 256 KiB blocks); with none left the job ends with OutOfMemory.
  V3dJobResult run_binning(const V3dBinJob& job, uint32_t attempts, ArrayView<const V3dMemoryBlock> overflow = ArrayView<const V3dMemoryBlock>());
  // Runs the renderer's control list [start, end) and waits for the frame-done bit.
  V3dJobResult run_rendering(uint32_t start, uint32_t end, uint32_t attempts);

  // Number of binning and rendering jobs the core has finished since reset (CLE_BFC and CLE_RFC, 8 bits each).
  uint32_t binning_jobs_done() const { return core_.read32(v3d::kCleBfc) & 0xFF; }
  uint32_t rendering_jobs_done() const { return core_.read32(v3d::kCleRfc) & 0xFF; }
  // The error and MMU interrupt bits seen by the last job that failed.
  uint32_t last_error() const { return last_error_; }
  uint32_t last_hub_interrupts() const { return last_hub_interrupts_; }

 private:
  V3dJobResult wait_for_list(uint32_t done_bit, uint32_t attempts, ArrayView<const V3dMemoryBlock> overflow);

  MmioRegion hub_, core_;
  V3dIdentity identity_;
  bool initialized_ = false;
  bool mmu_on_ = false;
  uint32_t last_error_ = 0, last_hub_interrupts_ = 0;
};

}  // namespace hardware
