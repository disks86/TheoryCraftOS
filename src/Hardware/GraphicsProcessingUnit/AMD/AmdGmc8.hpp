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
#include "Hardware/Clock/IClock.hpp"
#include "Hardware/Mmio.hpp"
#include "Hardware/VideoCodec/AMD/AmdGmc8Memory.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {

// The memory controller and the GPU page table (GART) of a Radeon RX Vega M, ported from the Linux amdgpu driver's gmc_v8_0.c (gmc_v8_0_hw_init: golden registers,
// gmc_v8_0_mc_program, gmc_v8_0_gart_enable, gmc_v8_0_gart_disable, the TLB flush) and amdgpu_gmc.c (where the driver puts the GART in the GPU's address space). The register
// numbers are generated from AMD's own headers (AmdGmc8Registers.inc, scripts/gen_vcn1_regs.py --vi).
//
// The card's firmware has already set the memory controller up, and AmdGmc8Memory only reads it. This is the driver's own set-up on top: the system aperture, the AGP
// window, the host data path, and VM context 0, which maps a window of the GPU's address space (the GART, 256 MiB) through a table of 8-byte entries in video memory
// onto any pages of video or system memory. That is what lets an engine reach a buffer that is not in the part of video memory the CPU can see, or in ordinary RAM.
// The other 15 contexts (per-process page tables) are set up empty, as the driver does before a process exists.
//
// Not here: the memory controller's own microcode (the Vega M needs none loaded by the driver: gmc_v8_0_polaris_mc_load_microcode is not run for VEGAM), VM fault
// handling (it needs the interrupt handler; faults are redirected to the dummy page), the VGA lockout (the driver does it only when it has a display driver; those registers belong to the display block), PRT apertures beyond the
// golden register values, and every check on a real card.
struct AmdGmc8GartPlan {
  bool ok = false;
  uint64_t vram_start = 0, vram_end = 0;          // the GPU's addresses of the first and last byte of video memory
  uint64_t gart_start = 0, gart_end = 0, gart_size = 0;
  uint64_t table_bytes = 0;                       // 8 bytes for each 4 KiB page of the GART
};

constexpr uint64_t kAmdGmc8McMask = 0xFFFFFFFFFFull;      // adev->gmc.mc_mask: the 40-bit address space of the memory controller
constexpr uint64_t kAmdGmc8DefaultGartBytes = 256ull << 20;   // gmc_v8_0_mc_init for VEGAM: amdgpu_gmc_set_gart_size(adev, SZ_256M)
constexpr uint64_t kAmdGmc8PageBytes = 4096;

// gmc_v8_0_vram_gtt_location: video memory where the controller says it is, the GART in the biggest free part of the address space (amdgpu_gmc_gart_location with the
// best-fit placement, aligned to 4 GiB; the size is reduced if it does not fit). Pure arithmetic. Not ok if the map was not read, the size is not whole pages or no space is left.
AmdGmc8GartPlan amd_plan_gmc8_gart(const AmdGmc8MemoryMap& map, uint64_t gart_bytes = kAmdGmc8DefaultGartBytes);

// The page table entry flags of VI (gmc_v8_0.c comment: "PTE format on VI"); the address is bits 39:12.
constexpr uint64_t kAmdGmc8PteValid = 1u << 0, kAmdGmc8PteSystem = 1u << 1, kAmdGmc8PteSnooped = 1u << 2, kAmdGmc8PteExecutable = 1u << 4, kAmdGmc8PteReadable = 1u << 5,
                   kAmdGmc8PteWriteable = 1u << 6;
constexpr uint64_t kAmdGmc8PteAddressMask = 0x000000FFFFFFF000ull;   // adev->gmc.pte_addr_mask
// What amdgpu_ttm_gart_bind uses for pages of ordinary memory (system, snooped by the CPU caches) and for pages of video memory, with the GART's executable bit.
constexpr uint64_t kAmdGmc8PteSystemPage = kAmdGmc8PteValid | kAmdGmc8PteSystem | kAmdGmc8PteSnooped | kAmdGmc8PteReadable | kAmdGmc8PteWriteable | kAmdGmc8PteExecutable;
constexpr uint64_t kAmdGmc8PteVideoPage = kAmdGmc8PteValid | kAmdGmc8PteReadable | kAmdGmc8PteWriteable | kAmdGmc8PteExecutable;

class AmdGmc8 {
 public:
  struct Config {
    // adev->vm_manager after amdgpu_vm_adjust_size(adev, 64, 9, 1, 40): the address space of a process (64 GiB at least; more on a machine with more than 21 GiB of RAM),
    // the page table block size and the L2 cache fragment size (log2 of pages).
    uint32_t vm_size_gb = 64;
    uint32_t block_size = 9;
    uint32_t fragment_size = 9;
  };
  static constexpr uint32_t kDefaultTimeoutUs = 100000;   // adev->usec_timeout
  static constexpr uint32_t kNumVmids = 16;               // AMDGPU_NUM_VMID

  // Memory the set-up needs: the table (page aligned, at least plan.table_bytes, in video memory the CPU can write), a page the memory controller redirects faults to
  // (the driver's dummy page), and the page unmapped system aperture accesses go to (adev->mem_scratch). Both are GPU addresses of pages the GPU may read.
  struct Memory {
    uint64_t table_gpu_address = 0;
    MmioRegion table;
    uint64_t dummy_page_gpu_address = 0;
    uint64_t scratch_page_gpu_address = 0;
  };
  enum class Result : uint8_t {
    Ok,
    BadArgument,   // the plan or the memory is not usable
    NotIdle,       // the memory controller never went idle (its busy bits in SRBM_STATUS stayed set)
  };
  static const char* result_name(Result result);

  AmdGmc8(IVcnRegisters& registers, const IClock& clock) : registers_(registers), clock_(clock) {}
  AmdGmc8(IVcnRegisters& registers, const IClock& clock, const Config& config) : registers_(registers), clock_(clock), config_(config) {}

  // gmc_v8_0_hw_init (golden registers, mc_program, gart_enable). The table is cleared first (every entry invalid).
  Result start(const AmdGmc8GartPlan& plan, const Memory& memory, uint32_t timeout_us = kDefaultTimeoutUs);
  // gmc_v8_0_gart_disable.
  void stop();

  // Maps the pages at `gpu_page_addresses` (each page aligned, inside the 40-bit space) to the GART pages from `first_page` on, with `pte_flags`, then flushes the host data
  // path and the GPU's TLBs. False if the range leaves the GART or an address is not a valid page (nothing is written then).
  bool map(uint64_t first_page, ArrayView<const uint64_t> gpu_page_addresses, uint64_t pte_flags);
  // Makes `count` GART pages invalid again.
  bool unmap(uint64_t first_page, uint64_t count);
  // The GPU address of a GART page (plan.gart_start + page * 4 KiB).
  uint64_t gart_address(uint64_t page) const { return plan_.gart_start + page * kAmdGmc8PageBytes; }
  // gmc_v8_0_flush_gpu_tlb(adev, vmid): bit `vmid` of VM_INVALIDATE_REQUEST.
  void flush_tlb(uint32_t vmid);

 private:
  uint32_t rd(uint32_t reg) { return registers_.read(reg); }
  void wr(uint32_t reg, uint32_t value) { registers_.write(reg, value); }
  bool wait_for_idle(uint32_t timeout_us);
  void program_golden_registers();
  bool mc_program(uint32_t timeout_us);
  void gart_enable();

  IVcnRegisters& registers_;
  const IClock& clock_;
  Config config_;
  AmdGmc8GartPlan plan_;
  Memory memory_;
  bool started_ = false;
};

}  // namespace hardware
