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


// Follows the Linux amdgpu driver (Copyright Advanced Micro Devices, Inc.; MIT license): gmc_v8_0.c (gmc_v8_0_init_golden_registers, _mc_program, _gart_enable,
// _gart_disable, _flush_gpu_tlb, _wait_for_idle, _vram_gtt_location) and amdgpu_gmc.c (amdgpu_gmc_gart_location, amdgpu_gmc_set_agp_default).
#include "Hardware/GraphicsProcessingUnit/AMD/AmdGmc8.hpp"

namespace hardware {

namespace {
// The register numbers and field masks this file uses, generated from gmc_8_1_d.h, gmc_8_1_sh_mask.h, bif_5_0_d.h, bif_5_0_sh_mask.h, oss_3_0_d.h and oss_3_0_sh_mask.h by
// scripts/gen_vcn1_regs.py --vi.
#include "Hardware/GraphicsProcessingUnit/AMD/AmdGmc8Registers.inc"

#define REG_SET_FIELD(orig, reg, field, val) (((orig) & ~reg##__##field##_MASK) | (reg##__##field##_MASK & ((val) << reg##__##field##__SHIFT)))

// golden_settings_polaris11_a11 (the table gmc_v8_0_init_golden_registers uses for the Polaris 11, 12 and the VEGAM): register, owned bits, value.
struct GoldenRegister {
  uint32_t reg, and_mask, or_mask;
};
constexpr GoldenRegister kGoldenVegam[] = {
    {mmVM_PRT_APERTURE0_LOW_ADDR, 0x0fffffff, 0x0fffffff},
    {mmVM_PRT_APERTURE1_LOW_ADDR, 0x0fffffff, 0x0fffffff},
    {mmVM_PRT_APERTURE2_LOW_ADDR, 0x0fffffff, 0x0fffffff},
    {mmVM_PRT_APERTURE3_LOW_ADDR, 0x0fffffff, 0x0fffffff},
};

constexpr uint64_t kFourGb = 1ull << 32;
constexpr uint32_t kBusyMask = SRBM_STATUS__MCB_BUSY_MASK | SRBM_STATUS__MCB_NON_DISPLAY_BUSY_MASK | SRBM_STATUS__MCC_BUSY_MASK | SRBM_STATUS__MCD_BUSY_MASK |
                               SRBM_STATUS__VMC_BUSY_MASK | SRBM_STATUS__VMC1_BUSY_MASK;
}  // namespace

const char* AmdGmc8::result_name(Result result) {
  switch (result) {
    case Result::Ok: return "ok";
    case Result::BadArgument: return "bad argument";
    case Result::NotIdle: return "memory controller busy";
  }
  return "?";
}

AmdGmc8GartPlan amd_plan_gmc8_gart(const AmdGmc8MemoryMap& map, uint64_t gart_bytes) {
  AmdGmc8GartPlan plan;
  if (!map.read || map.video_memory_bytes == 0 || gart_bytes == 0 || (gart_bytes & (kAmdGmc8PageBytes - 1)) != 0) return plan;
  // The video memory (amdgpu_gmc_vram_location): it ends where its size says, not where the controller's rounded-up top is.
  if (map.gpu_base > kAmdGmc8McMask || map.video_memory_bytes > kAmdGmc8McMask + 1 - map.gpu_base) return plan;
  const uint64_t fb_start = map.gpu_base, fb_end = map.gpu_base + map.video_memory_bytes - 1;
  // amdgpu_gmc_gart_location: the space before and after video memory (the GART is aligned to 4 GiB, as the video engines do not like a buffer across such a boundary).
  const uint64_t max_mc_address = kAmdGmc8McMask;
  const uint64_t after_start = (fb_end + 1 + kFourGb - 1) / kFourGb * kFourGb;
  const uint64_t size_bf = fb_start, size_af = after_start > max_mc_address ? 0 : max_mc_address + 1 - after_start;
  uint64_t size = gart_bytes;
  const uint64_t room = size_bf > size_af ? size_bf : size_af;
  if (size > room) size = room & ~(kAmdGmc8PageBytes - 1);
  if (size == 0) return plan;
  uint64_t start;
  if ((size_bf >= size && size_bf < size_af) || size_af < size) start = 0;
  else start = max_mc_address - size + 1;
  start &= ~(kFourGb - 1);
  plan.vram_start = fb_start;
  plan.vram_end = fb_end;
  plan.gart_start = start;
  plan.gart_size = size;
  plan.gart_end = start + size - 1;
  plan.table_bytes = size / kAmdGmc8PageBytes * 8;
  plan.ok = true;
  return plan;
}

bool AmdGmc8::wait_for_idle(uint32_t timeout_us) {
  const uint64_t begin = clock_.now_us();
  do {
    if ((rd(mmSRBM_STATUS) & kBusyMask) == 0) return true;
  } while (clock_.now_us() - begin < timeout_us);
  return false;
}

// amdgpu_device_program_register_sequence for the families before AI.
void AmdGmc8::program_golden_registers() {
  for (const GoldenRegister& g : kGoldenVegam) {
    uint32_t value = g.or_mask;
    if (g.and_mask != 0xFFFFFFFFu) value = (rd(g.reg) & ~g.and_mask) | g.or_mask;
    wr(g.reg, value);
  }
}

bool AmdGmc8::mc_program(uint32_t timeout_us) {
  // Initialize HDP
  for (uint32_t i = 0, j = 0; i < 32; ++i, j += 0x6) {
    wr(0xb05 + j, 0x00000000);
    wr(0xb06 + j, 0x00000000);
    wr(0xb07 + j, 0x00000000);
    wr(0xb08 + j, 0x00000000);
    wr(0xb09 + j, 0x00000000);
  }
  wr(mmHDP_REG_COHERENCY_FLUSH_CNTL, 0);
  bool idle = wait_for_idle(timeout_us);   // (the driver only warns)
  // Update configuration
  wr(mmMC_VM_SYSTEM_APERTURE_LOW_ADDR, static_cast<uint32_t>(plan_.vram_start >> 12));
  wr(mmMC_VM_SYSTEM_APERTURE_HIGH_ADDR, static_cast<uint32_t>(plan_.vram_end >> 12));
  wr(mmMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR, static_cast<uint32_t>(memory_.scratch_page_gpu_address >> 12));
  // amdgpu_gmc_set_agp_default: no AGP window (start 0xffffffffffff, end 0).
  wr(mmMC_VM_AGP_BASE, 0);
  wr(mmMC_VM_AGP_TOP, static_cast<uint32_t>(uint64_t{0} >> 22));
  wr(mmMC_VM_AGP_BOT, static_cast<uint32_t>(uint64_t{0xffffffffffffull} >> 22));
  idle = wait_for_idle(timeout_us) && idle;
  wr(mmBIF_FB_EN, BIF_FB_EN__FB_READ_EN_MASK | BIF_FB_EN__FB_WRITE_EN_MASK);
  uint32_t tmp = rd(mmHDP_MISC_CNTL);
  tmp = REG_SET_FIELD(tmp, HDP_MISC_CNTL, FLUSH_INVALIDATE_CACHE, 0);
  wr(mmHDP_MISC_CNTL, tmp);
  tmp = rd(mmHDP_HOST_PATH_CNTL);
  wr(mmHDP_HOST_PATH_CNTL, tmp);
  return idle;
}

void AmdGmc8::gart_enable() {
  const uint64_t table_addr = memory_.table_gpu_address;
  // Setup TLB control
  uint32_t tmp = rd(mmMC_VM_MX_L1_TLB_CNTL);
  tmp = REG_SET_FIELD(tmp, MC_VM_MX_L1_TLB_CNTL, ENABLE_L1_TLB, 1);
  tmp = REG_SET_FIELD(tmp, MC_VM_MX_L1_TLB_CNTL, ENABLE_L1_FRAGMENT_PROCESSING, 1);
  tmp = REG_SET_FIELD(tmp, MC_VM_MX_L1_TLB_CNTL, SYSTEM_ACCESS_MODE, 3);
  tmp = REG_SET_FIELD(tmp, MC_VM_MX_L1_TLB_CNTL, ENABLE_ADVANCED_DRIVER_MODEL, 1);
  tmp = REG_SET_FIELD(tmp, MC_VM_MX_L1_TLB_CNTL, SYSTEM_APERTURE_UNMAPPED_ACCESS, 0);
  wr(mmMC_VM_MX_L1_TLB_CNTL, tmp);
  // Setup L2 cache
  tmp = rd(mmVM_L2_CNTL);
  tmp = REG_SET_FIELD(tmp, VM_L2_CNTL, ENABLE_L2_CACHE, 1);
  tmp = REG_SET_FIELD(tmp, VM_L2_CNTL, ENABLE_L2_FRAGMENT_PROCESSING, 1);
  tmp = REG_SET_FIELD(tmp, VM_L2_CNTL, ENABLE_L2_PTE_CACHE_LRU_UPDATE_BY_WRITE, 1);
  tmp = REG_SET_FIELD(tmp, VM_L2_CNTL, ENABLE_L2_PDE0_CACHE_LRU_UPDATE_BY_WRITE, 1);
  tmp = REG_SET_FIELD(tmp, VM_L2_CNTL, EFFECTIVE_L2_QUEUE_SIZE, 7);
  tmp = REG_SET_FIELD(tmp, VM_L2_CNTL, CONTEXT1_IDENTITY_ACCESS_MODE, 1);
  tmp = REG_SET_FIELD(tmp, VM_L2_CNTL, ENABLE_DEFAULT_PAGE_OUT_TO_SYSTEM_MEMORY, 1);
  wr(mmVM_L2_CNTL, tmp);
  tmp = rd(mmVM_L2_CNTL2);
  tmp = REG_SET_FIELD(tmp, VM_L2_CNTL2, INVALIDATE_ALL_L1_TLBS, 1);
  tmp = REG_SET_FIELD(tmp, VM_L2_CNTL2, INVALIDATE_L2_CACHE, 1);
  wr(mmVM_L2_CNTL2, tmp);

  const uint32_t field = config_.fragment_size;
  tmp = rd(mmVM_L2_CNTL3);
  tmp = REG_SET_FIELD(tmp, VM_L2_CNTL3, L2_CACHE_BIGK_ASSOCIATIVITY, 1);
  tmp = REG_SET_FIELD(tmp, VM_L2_CNTL3, BANK_SELECT, field);
  tmp = REG_SET_FIELD(tmp, VM_L2_CNTL3, L2_CACHE_BIGK_FRAGMENT_SIZE, field);
  wr(mmVM_L2_CNTL3, tmp);
  // XXX: set to enable PTE/PDE in system memory
  tmp = rd(mmVM_L2_CNTL4);
  tmp = REG_SET_FIELD(tmp, VM_L2_CNTL4, VMC_TAP_CONTEXT0_PDE_REQUEST_PHYSICAL, 0);
  tmp = REG_SET_FIELD(tmp, VM_L2_CNTL4, VMC_TAP_CONTEXT0_PDE_REQUEST_SHARED, 0);
  tmp = REG_SET_FIELD(tmp, VM_L2_CNTL4, VMC_TAP_CONTEXT0_PDE_REQUEST_SNOOP, 0);
  tmp = REG_SET_FIELD(tmp, VM_L2_CNTL4, VMC_TAP_CONTEXT0_PTE_REQUEST_PHYSICAL, 0);
  tmp = REG_SET_FIELD(tmp, VM_L2_CNTL4, VMC_TAP_CONTEXT0_PTE_REQUEST_SHARED, 0);
  tmp = REG_SET_FIELD(tmp, VM_L2_CNTL4, VMC_TAP_CONTEXT0_PTE_REQUEST_SNOOP, 0);
  tmp = REG_SET_FIELD(tmp, VM_L2_CNTL4, VMC_TAP_CONTEXT1_PDE_REQUEST_PHYSICAL, 0);
  tmp = REG_SET_FIELD(tmp, VM_L2_CNTL4, VMC_TAP_CONTEXT1_PDE_REQUEST_SHARED, 0);
  tmp = REG_SET_FIELD(tmp, VM_L2_CNTL4, VMC_TAP_CONTEXT1_PDE_REQUEST_SNOOP, 0);
  tmp = REG_SET_FIELD(tmp, VM_L2_CNTL4, VMC_TAP_CONTEXT1_PTE_REQUEST_PHYSICAL, 0);
  tmp = REG_SET_FIELD(tmp, VM_L2_CNTL4, VMC_TAP_CONTEXT1_PTE_REQUEST_SHARED, 0);
  tmp = REG_SET_FIELD(tmp, VM_L2_CNTL4, VMC_TAP_CONTEXT1_PTE_REQUEST_SNOOP, 0);
  wr(mmVM_L2_CNTL4, tmp);
  // setup context0
  wr(mmVM_CONTEXT0_PAGE_TABLE_START_ADDR, static_cast<uint32_t>(plan_.gart_start >> 12));
  wr(mmVM_CONTEXT0_PAGE_TABLE_END_ADDR, static_cast<uint32_t>(plan_.gart_end >> 12));
  wr(mmVM_CONTEXT0_PAGE_TABLE_BASE_ADDR, static_cast<uint32_t>(table_addr >> 12));
  wr(mmVM_CONTEXT0_PROTECTION_FAULT_DEFAULT_ADDR, static_cast<uint32_t>(memory_.dummy_page_gpu_address >> 12));
  wr(mmVM_CONTEXT0_CNTL2, 0);
  tmp = rd(mmVM_CONTEXT0_CNTL);
  tmp = REG_SET_FIELD(tmp, VM_CONTEXT0_CNTL, ENABLE_CONTEXT, 1);
  tmp = REG_SET_FIELD(tmp, VM_CONTEXT0_CNTL, PAGE_TABLE_DEPTH, 0);
  tmp = REG_SET_FIELD(tmp, VM_CONTEXT0_CNTL, RANGE_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
  wr(mmVM_CONTEXT0_CNTL, tmp);

  wr(mmVM_L2_CONTEXT1_IDENTITY_APERTURE_LOW_ADDR, 0);
  wr(mmVM_L2_CONTEXT1_IDENTITY_APERTURE_HIGH_ADDR, 0);
  wr(mmVM_L2_CONTEXT_IDENTITY_PHYSICAL_OFFSET, 0);

  // empty context1-15: the size of the address space, and the GART's table as their base until a process has its own
  const uint64_t max_pfn = uint64_t{config_.vm_size_gb} << 18;
  wr(mmVM_CONTEXT1_PAGE_TABLE_START_ADDR, 0);
  wr(mmVM_CONTEXT1_PAGE_TABLE_END_ADDR, static_cast<uint32_t>(max_pfn - 1));
  for (uint32_t i = 1; i < kNumVmids; ++i) {
    if (i < 8) wr(mmVM_CONTEXT0_PAGE_TABLE_BASE_ADDR + i, static_cast<uint32_t>(table_addr >> 12));
    else wr(mmVM_CONTEXT8_PAGE_TABLE_BASE_ADDR + i - 8, static_cast<uint32_t>(table_addr >> 12));
  }

  // enable context1-15
  wr(mmVM_CONTEXT1_PROTECTION_FAULT_DEFAULT_ADDR, static_cast<uint32_t>(memory_.dummy_page_gpu_address >> 12));
  wr(mmVM_CONTEXT1_CNTL2, 4);
  tmp = rd(mmVM_CONTEXT1_CNTL);
  tmp = REG_SET_FIELD(tmp, VM_CONTEXT1_CNTL, ENABLE_CONTEXT, 1);
  tmp = REG_SET_FIELD(tmp, VM_CONTEXT1_CNTL, PAGE_TABLE_DEPTH, 1);
  tmp = REG_SET_FIELD(tmp, VM_CONTEXT1_CNTL, RANGE_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
  tmp = REG_SET_FIELD(tmp, VM_CONTEXT1_CNTL, DUMMY_PAGE_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
  tmp = REG_SET_FIELD(tmp, VM_CONTEXT1_CNTL, PDE0_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
  tmp = REG_SET_FIELD(tmp, VM_CONTEXT1_CNTL, VALID_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
  tmp = REG_SET_FIELD(tmp, VM_CONTEXT1_CNTL, READ_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
  tmp = REG_SET_FIELD(tmp, VM_CONTEXT1_CNTL, WRITE_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
  tmp = REG_SET_FIELD(tmp, VM_CONTEXT1_CNTL, EXECUTE_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
  tmp = REG_SET_FIELD(tmp, VM_CONTEXT1_CNTL, PAGE_TABLE_BLOCK_SIZE, config_.block_size - 9);
  wr(mmVM_CONTEXT1_CNTL, tmp);
  // gmc_v8_0_set_fault_enable_default(adev, true): the same bits again (faults are redirected to the dummy page)
  tmp = rd(mmVM_CONTEXT1_CNTL);
  tmp = REG_SET_FIELD(tmp, VM_CONTEXT1_CNTL, RANGE_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
  tmp = REG_SET_FIELD(tmp, VM_CONTEXT1_CNTL, DUMMY_PAGE_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
  tmp = REG_SET_FIELD(tmp, VM_CONTEXT1_CNTL, PDE0_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
  tmp = REG_SET_FIELD(tmp, VM_CONTEXT1_CNTL, VALID_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
  tmp = REG_SET_FIELD(tmp, VM_CONTEXT1_CNTL, READ_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
  tmp = REG_SET_FIELD(tmp, VM_CONTEXT1_CNTL, WRITE_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
  tmp = REG_SET_FIELD(tmp, VM_CONTEXT1_CNTL, EXECUTE_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
  wr(mmVM_CONTEXT1_CNTL, tmp);

  flush_tlb(0);
}

void AmdGmc8::flush_tlb(uint32_t vmid) { wr(mmVM_INVALIDATE_REQUEST, 1u << vmid); }

AmdGmc8::Result AmdGmc8::start(const AmdGmc8GartPlan& plan, const Memory& memory, uint32_t timeout_us) {
  if (!plan.ok || !memory.table.valid() || memory.table.length() < plan.table_bytes || (memory.table_gpu_address & (kAmdGmc8PageBytes - 1)) != 0 ||
      (memory.table.address() & (kAmdGmc8PageBytes - 1)) != 0 || memory.table_gpu_address > kAmdGmc8McMask || (memory.dummy_page_gpu_address & (kAmdGmc8PageBytes - 1)) != 0 ||
      memory.dummy_page_gpu_address > kAmdGmc8McMask || (memory.scratch_page_gpu_address & (kAmdGmc8PageBytes - 1)) != 0 || memory.scratch_page_gpu_address > kAmdGmc8McMask ||
      config_.block_size < 9 || config_.vm_size_gb == 0 || config_.fragment_size > 0xFF)
    return Result::BadArgument;
  plan_ = plan;
  memory_ = memory;
  // amdgpu_gart_table_vram_alloc hands out a cleared table: every page invalid.
  for (uint64_t o = 0; o < plan.table_bytes; o += 8) memory.table.write64(o, 0);
  amd_gmc8_flush_hdp(registers_);
  // gmc_v8_0_hw_init
  program_golden_registers();
  const bool idle = mc_program(timeout_us);
  gart_enable();
  started_ = true;
  return idle ? Result::Ok : Result::NotIdle;
}

void AmdGmc8::stop() {
  // Disable all tables
  wr(mmVM_CONTEXT0_CNTL, 0);
  wr(mmVM_CONTEXT1_CNTL, 0);
  // Setup TLB control
  uint32_t tmp = rd(mmMC_VM_MX_L1_TLB_CNTL);
  tmp = REG_SET_FIELD(tmp, MC_VM_MX_L1_TLB_CNTL, ENABLE_L1_TLB, 0);
  tmp = REG_SET_FIELD(tmp, MC_VM_MX_L1_TLB_CNTL, ENABLE_L1_FRAGMENT_PROCESSING, 0);
  tmp = REG_SET_FIELD(tmp, MC_VM_MX_L1_TLB_CNTL, ENABLE_ADVANCED_DRIVER_MODEL, 0);
  wr(mmMC_VM_MX_L1_TLB_CNTL, tmp);
  // Setup L2 cache
  tmp = rd(mmVM_L2_CNTL);
  tmp = REG_SET_FIELD(tmp, VM_L2_CNTL, ENABLE_L2_CACHE, 0);
  wr(mmVM_L2_CNTL, tmp);
  wr(mmVM_L2_CNTL2, 0);
  started_ = false;
}

bool AmdGmc8::map(uint64_t first_page, ArrayView<const uint64_t> gpu_page_addresses, uint64_t pte_flags) {
  const uint64_t pages = plan_.gart_size / kAmdGmc8PageBytes;
  if (!started_ || first_page >= pages || gpu_page_addresses.size() > pages - first_page || (pte_flags & kAmdGmc8PteAddressMask) != 0 || (pte_flags & kAmdGmc8PteValid) == 0) return false;
  for (uint64_t address : gpu_page_addresses)
    if ((address & ~kAmdGmc8PteAddressMask) != 0) return false;   // not page aligned, or beyond the 40-bit space
  uint64_t offset = first_page * 8;
  for (uint64_t address : gpu_page_addresses) {
    memory_.table.write64(offset, address | pte_flags);
    offset += 8;
  }
  amd_gmc8_flush_hdp(registers_);
  flush_tlb(0);
  return true;
}

bool AmdGmc8::unmap(uint64_t first_page, uint64_t count) {
  const uint64_t pages = plan_.gart_size / kAmdGmc8PageBytes;
  if (!started_ || first_page >= pages || count > pages - first_page) return false;
  for (uint64_t i = 0; i < count; ++i) memory_.table.write64((first_page + i) * 8, 0);
  amd_gmc8_flush_hdp(registers_);
  flush_tlb(0);
  return true;
}

}  // namespace hardware
