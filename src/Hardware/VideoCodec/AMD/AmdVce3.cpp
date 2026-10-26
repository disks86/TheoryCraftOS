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


// Ported from the Linux kernel's drivers/gpu/drm/amd/amdgpu/vce_v3_0.c and amdgpu_vce.c (Copyright 2013 Advanced Micro Devices, Inc.; MIT license). The register names and the
// order of the writes are the driver's: the macros below are the driver's own so each function can be read side by side with its original.
// Differences from the driver are marked "not ported:".
#include "Hardware/VideoCodec/AMD/AmdVce3.hpp"

namespace hardware {

namespace {
// Defined in vce_v3_0.c itself rather than in AMD's headers.
constexpr uint32_t mmVCE_LMI_VCPU_CACHE_40BIT_BAR0 = 0x8616;
constexpr uint32_t mmVCE_LMI_VCPU_CACHE_40BIT_BAR1 = 0x8617;
constexpr uint32_t mmVCE_LMI_VCPU_CACHE_40BIT_BAR2 = 0x8618;
constexpr uint32_t mmGRBM_GFX_INDEX_DEFAULT = 0xE0000000;
constexpr uint32_t GRBM_GFX_INDEX__VCE_INSTANCE__SHIFT = 0x04;
constexpr uint32_t GRBM_GFX_INDEX__VCE_ALL_PIPE = 0x07;
constexpr uint32_t VCE_STATUS_VCPU_REPORT_FW_LOADED_MASK = 0x02;

// The register numbers and field masks this file uses, generated from vce_3_0_d.h, vce_3_0_sh_mask.h, gfx_8_0_d.h, smu_7_1_2_d.h, smu_7_1_2_sh_mask.h and bif_5_0_d.h by
// scripts/gen_vcn1_regs.py --vi.
#include "Hardware/VideoCodec/AMD/AmdVce3Registers.inc"

constexpr uint32_t lower_32_bits(uint64_t v) { return static_cast<uint32_t>(v); }
constexpr uint32_t upper_32_bits(uint64_t v) { return static_cast<uint32_t>(v >> 32); }
constexpr uint32_t FW_52_8_3 = (52u << 24) | (8u << 16) | (3u << 8);
constexpr uint32_t GET_VCE_INSTANCE(uint32_t i) { return (i << GRBM_GFX_INDEX__VCE_INSTANCE__SHIFT) | GRBM_GFX_INDEX__VCE_ALL_PIPE; }
}  // namespace

#define RREG32(reg) rd(reg)
#define WREG32(reg, value) wr(reg, (value))
#define WREG32_P(reg, val, mask) do { uint32_t t_ = rd(reg); t_ &= (mask); t_ |= ((val) & ~(mask)); wr(reg, t_); } while (0)
#define WREG32_OR(reg, or_) WREG32_P(reg, or_, ~(or_))
#define REG_FIELD_SHIFT(reg, field) reg##__##field##__SHIFT
#define REG_FIELD_MASK(reg, field) reg##__##field##_MASK
#define WREG32_FIELD(reg, field, val) wr(mm##reg, (rd(mm##reg) & ~REG_FIELD_MASK(reg, field)) | ((val) << REG_FIELD_SHIFT(reg, field)))
#define mdelay(ms) delay_ms(ms)

const char* amd_vce3_firmware_name(uint16_t pci_device_id) {
  // amdgpu_vce.c: CHIP_VEGAM -> "amdgpu/vegam_vce.bin"; amdgpu_drv.c lists 0x694C, 0x694E and 0x694F as CHIP_VEGAM.
  return pci_device_id == 0x694C || pci_device_id == 0x694E || pci_device_id == 0x694F ? "vegam_vce.bin" : nullptr;
}

void AmdVce3::delay_ms(uint32_t ms) {
  const uint64_t begin = clock_.now_us();
  while (clock_.now_us() - begin < uint64_t{ms} * 1000) {}
}

bool AmdVce3::firmware_usable(const AmdVcnFirmware& firmware) {
  return firmware.loaded() && firmware.header().ip_version_major == 3 && firmware.ucode().size() > 0 && firmware.ucode().size() <= kFirmwareWindowBytes;
}

uint32_t AmdVce3::firmware_version(const AmdVcnFirmware& firmware) {
  // amdgpu_vce_sw_init: the version word of the header is (major << 20 | minor << 8 | binary id), the driver keeps (major << 24 | minor << 16 | binary id << 8).
  const uint32_t ucode_version = firmware.header().ucode_version;
  const uint32_t version_major = (ucode_version >> 20) & 0xfff, version_minor = (ucode_version >> 8) & 0xfff, binary_id = ucode_version & 0xff;
  return (version_major << 24) | (version_minor << 16) | (binary_id << 8);
}

// vce_v3_0_sw_init: "52.8.3 required for 3 ring support"
uint32_t AmdVce3::rings_for(uint32_t firmware_version) { return firmware_version < FW_52_8_3 ? 2 : 3; }

// RREG32_SMC (vi.c): the SMC's registers are reached through an index and a data register.
static uint32_t read_smc(IVcnRegisters& registers, uint32_t reg) {
  registers.write(mmSMC_IND_INDEX_0, reg);
  return registers.read(mmSMC_IND_DATA_0);
}

// vce_v3_0_get_harvest_config (a discrete Polaris-family card; the APU branch, the Fiji and Stoney special cases and the "no engine at all" early_init check are not ported)
uint32_t AmdVce3::read_harvest_config(IVcnRegisters& registers) {
  uint32_t tmp;

  tmp = (read_smc(registers, ixCC_HARVEST_FUSES) & CC_HARVEST_FUSES__VCE_DISABLE_MASK) >> CC_HARVEST_FUSES__VCE_DISABLE__SHIFT;

  switch (tmp) {
  case 1:
    return kHarvestVce0;
  case 2:
    return kHarvestVce1;
  case 3:
    return kHarvestVce0 | kHarvestVce1;
  default:
    return kHarvestVce1;   // (POLARIS10, POLARIS11, POLARIS12 and VEGAM)
  }
}

AmdVce3::Diagnostics AmdVce3::read_diagnostics(IVcnRegisters& registers, uint32_t harvest_config) {
  // The first instance the start programs; the instance is selected through GRBM_GFX_INDEX and the default put back (reads and the one selecting write only).
  const uint32_t first = (harvest_config & kHarvestVce0) ? 1 : 0;
  Diagnostics d;
  registers.write(mmGRBM_GFX_INDEX, GET_VCE_INSTANCE(first));
  d.status = registers.read(mmVCE_STATUS);
  d.soft_reset = registers.read(mmVCE_SOFT_RESET);
  d.vcpu_cntl = registers.read(mmVCE_VCPU_CNTL);
  d.lmi_ctrl = registers.read(mmVCE_LMI_CTRL);
  d.lmi_ctrl2 = registers.read(mmVCE_LMI_CTRL2);
  d.rb_rptr = registers.read(mmVCE_RB_RPTR);
  d.rb_wptr = registers.read(mmVCE_RB_WPTR);
  registers.write(mmGRBM_GFX_INDEX, mmGRBM_GFX_INDEX_DEFAULT);
  return d;
}

// vce_v3_0_override_vce_clock_gating
void AmdVce3::override_vce_clock_gating(bool override) {
  WREG32_FIELD(VCE_RB_ARB_CTRL, VCE_CGTT_OVERRIDE, override ? 1 : 0);
}

// vce_v3_0_set_vce_sw_clock_gating
void AmdVce3::set_vce_sw_clock_gating(bool gated) {
  uint32_t data;

  /* Set Override to disable Clock Gating */
  override_vce_clock_gating(true);

  /* This function enables MGCG which is controlled by firmware.
     With the clocks in the gated state the core is still
     accessible but the firmware will throttle the clocks on the
     fly as necessary.
  */
  if (!gated) {
    data = RREG32(mmVCE_CLOCK_GATING_B);
    data |= 0x1ff;
    data &= ~0xef0000;
    WREG32(mmVCE_CLOCK_GATING_B, data);

    data = RREG32(mmVCE_UENC_CLOCK_GATING);
    data |= 0x3ff000;
    data &= ~0xffc00000;
    WREG32(mmVCE_UENC_CLOCK_GATING, data);

    data = RREG32(mmVCE_UENC_CLOCK_GATING_2);
    data |= 0x2;
    data &= ~0x00010000;
    WREG32(mmVCE_UENC_CLOCK_GATING_2, data);

    data = RREG32(mmVCE_UENC_REG_CLOCK_GATING);
    data |= 0x37f;
    WREG32(mmVCE_UENC_REG_CLOCK_GATING, data);

    data = RREG32(mmVCE_UENC_DMA_DCLK_CTRL);
    data |= VCE_UENC_DMA_DCLK_CTRL__WRDMCLK_FORCEON_MASK |
      VCE_UENC_DMA_DCLK_CTRL__RDDMCLK_FORCEON_MASK |
      VCE_UENC_DMA_DCLK_CTRL__REGCLK_FORCEON_MASK  |
      0x8;
    WREG32(mmVCE_UENC_DMA_DCLK_CTRL, data);
  } else {
    data = RREG32(mmVCE_CLOCK_GATING_B);
    data &= ~0x80010;
    data |= 0xe70008;
    WREG32(mmVCE_CLOCK_GATING_B, data);

    data = RREG32(mmVCE_UENC_CLOCK_GATING);
    data |= 0xffc00000;
    WREG32(mmVCE_UENC_CLOCK_GATING, data);

    data = RREG32(mmVCE_UENC_CLOCK_GATING_2);
    data |= 0x10000;
    WREG32(mmVCE_UENC_CLOCK_GATING_2, data);

    data = RREG32(mmVCE_UENC_REG_CLOCK_GATING);
    data &= ~0x3ff;
    WREG32(mmVCE_UENC_REG_CLOCK_GATING, data);

    data = RREG32(mmVCE_UENC_DMA_DCLK_CTRL);
    data &= ~(VCE_UENC_DMA_DCLK_CTRL__WRDMCLK_FORCEON_MASK |
        VCE_UENC_DMA_DCLK_CTRL__RDDMCLK_FORCEON_MASK |
        VCE_UENC_DMA_DCLK_CTRL__REGCLK_FORCEON_MASK  |
        0x8);
    WREG32(mmVCE_UENC_DMA_DCLK_CTRL, data);
  }

  override_vce_clock_gating(false);
}

// vce_v3_0_set_clockgating_state (with the check of cg_flags & AMD_CG_SUPPORT_VCE_MGCG made by the caller)
void AmdVce3::set_clockgating_state(bool enable) {
  int i;

  for (i = 0; i < 2; i++) {
    /* Program VCE Instance 0 or 1 if not harvested */
    if (config_.harvest_config & (1 << i))
      continue;

    WREG32(mmGRBM_GFX_INDEX, GET_VCE_INSTANCE(i));

    if (!enable) {
      /* initialize VCE_CLOCK_GATING_A: Clock ON/OFF delay */
      uint32_t data = RREG32(mmVCE_CLOCK_GATING_A);
      data &= ~(0xf | 0xff0);
      data |= ((0x0 << 0) | (0x04 << 4));
      WREG32(mmVCE_CLOCK_GATING_A, data);

      /* initialize VCE_UENC_CLOCK_GATING: Clock ON/OFF delay */
      data = RREG32(mmVCE_UENC_CLOCK_GATING);
      data &= ~(0xf | 0xff0);
      data |= ((0x0 << 0) | (0x04 << 4));
      WREG32(mmVCE_UENC_CLOCK_GATING, data);
    }

    set_vce_sw_clock_gating(enable);
  }

  WREG32(mmGRBM_GFX_INDEX, mmGRBM_GFX_INDEX_DEFAULT);
}

// vce_v3_0_mc_resume
void AmdVce3::mc_resume(const Memory& memory, uint32_t idx) {
  uint32_t offset, size;

  WREG32_P(mmVCE_CLOCK_GATING_A, 0, ~(1 << 16));
  WREG32_P(mmVCE_UENC_CLOCK_GATING, 0x1FF000, ~0xFF9FF000);
  WREG32_P(mmVCE_UENC_REG_CLOCK_GATING, 0x3F, ~0x3F);
  WREG32(mmVCE_CLOCK_GATING_B, 0x1FF);

  WREG32(mmVCE_LMI_CTRL, 0x00398000);
  WREG32_P(mmVCE_LMI_CACHE_CTRL, 0x0, ~0x1);
  WREG32(mmVCE_LMI_SWAP_CNTL, 0);
  WREG32(mmVCE_LMI_SWAP_CNTL1, 0);
  WREG32(mmVCE_LMI_VM_CTRL, 0);
  WREG32_OR(mmVCE_VCPU_CNTL, 0x00100000);

  /* (the chips before Stoney have the single mmVCE_LMI_VCPU_CACHE_40BIT_BAR: not ported) */
  WREG32(mmVCE_LMI_VCPU_CACHE_40BIT_BAR0, (memory.block_gpu_address >> 8));
  WREG32(mmVCE_LMI_VCPU_CACHE_40BIT_BAR1, (memory.block_gpu_address >> 8));
  WREG32(mmVCE_LMI_VCPU_CACHE_40BIT_BAR2, (memory.block_gpu_address >> 8));
  offset = kFirmwareOffset;
  size = kFirmwareWindowBytes;
  WREG32(mmVCE_VCPU_CACHE_OFFSET0, offset & 0x7fffffff);
  WREG32(mmVCE_VCPU_CACHE_SIZE0, size);

  if (idx == 0) {
    offset += size;
    size = kStackBytes;
    WREG32(mmVCE_VCPU_CACHE_OFFSET1, offset & 0x7fffffff);
    WREG32(mmVCE_VCPU_CACHE_SIZE1, size);
    offset += size;
    size = kDataBytes;
    WREG32(mmVCE_VCPU_CACHE_OFFSET2, offset & 0x7fffffff);
    WREG32(mmVCE_VCPU_CACHE_SIZE2, size);
  } else {
    offset += size + kStackBytes + kDataBytes;
    size = kStackBytes;
    WREG32(mmVCE_VCPU_CACHE_OFFSET1, offset & 0xfffffff);
    WREG32(mmVCE_VCPU_CACHE_SIZE1, size);
    offset += size;
    size = kDataBytes;
    WREG32(mmVCE_VCPU_CACHE_OFFSET2, offset & 0xfffffff);
    WREG32(mmVCE_VCPU_CACHE_SIZE2, size);
  }

  WREG32_P(mmVCE_LMI_CTRL2, 0x0, ~0x100);
  WREG32_FIELD(VCE_SYS_INT_EN, VCE_SYS_INT_TRAP_INTERRUPT_EN, 1);
}

// vce_v3_0_firmware_loaded (the driver's ten tries of 100 x 10 ms are config_.boot_attempts)
bool AmdVce3::firmware_loaded() {
  int i, j;

  for (i = 0; i < static_cast<int>(config_.boot_attempts ? config_.boot_attempts : 1); ++i) {
    for (j = 0; j < 100; ++j) {
      uint32_t status = RREG32(mmVCE_STATUS);
      if (status & VCE_STATUS_VCPU_REPORT_FW_LOADED_MASK)
        return true;
      mdelay(10);
    }

    /* VCE not responding, trying to reset the ECPU */
    WREG32_FIELD(VCE_SOFT_RESET, ECPU_SOFT_RESET, 1);
    mdelay(10);
    WREG32_FIELD(VCE_SOFT_RESET, ECPU_SOFT_RESET, 0);
    mdelay(10);
  }

  return false;
}

// vce_v3_0_start (with the ring memory of the driver's three rings; the rings of a second instance are not programmed unless it is the only one, as in the driver)
AmdVce3::Status AmdVce3::run_start(const Memory& memory) {
  int idx;
  bool r;

  for (idx = 0; idx < 2; ++idx) {
    if (config_.harvest_config & (1 << idx))
      continue;

    WREG32(mmGRBM_GFX_INDEX, GET_VCE_INSTANCE(idx));

    /* Program instance 0 reg space for two instances or instance 0 case
    program instance 1 reg space for only instance 1 available case */
    if (idx != 1 || config_.harvest_config == kHarvestVce0) {
      // ring[0]
      uint32_t gpu = lower_32_bits(memory.ring_gpu_address[0]), gpu_high = upper_32_bits(memory.ring_gpu_address[0]), wptr = config_.ring_write_pointer[0];
      WREG32(mmVCE_RB_RPTR, wptr);
      WREG32(mmVCE_RB_WPTR, wptr);
      WREG32(mmVCE_RB_BASE_LO, gpu);
      WREG32(mmVCE_RB_BASE_HI, gpu_high);
      WREG32(mmVCE_RB_SIZE, kRingBytes / 4);

      // ring[1]
      gpu = lower_32_bits(memory.ring_gpu_address[1]);
      gpu_high = upper_32_bits(memory.ring_gpu_address[1]);
      wptr = config_.ring_write_pointer[1];
      WREG32(mmVCE_RB_RPTR2, wptr);
      WREG32(mmVCE_RB_WPTR2, wptr);
      WREG32(mmVCE_RB_BASE_LO2, gpu);
      WREG32(mmVCE_RB_BASE_HI2, gpu_high);
      WREG32(mmVCE_RB_SIZE2, kRingBytes / 4);

      // ring[2] (the driver writes it whatever the ring count is: with two rings it is the ring structure that was never initialised, an address and size of zero)
      const bool third = config_.rings >= 3;
      gpu = third ? lower_32_bits(memory.ring_gpu_address[2]) : 0;
      gpu_high = third ? upper_32_bits(memory.ring_gpu_address[2]) : 0;
      wptr = third ? config_.ring_write_pointer[2] : 0;
      WREG32(mmVCE_RB_RPTR3, wptr);
      WREG32(mmVCE_RB_WPTR3, wptr);
      WREG32(mmVCE_RB_BASE_LO3, gpu);
      WREG32(mmVCE_RB_BASE_HI3, gpu_high);
      WREG32(mmVCE_RB_SIZE3, third ? kRingBytes / 4 : 0);
    }

    mc_resume(memory, idx);
    WREG32_FIELD(VCE_STATUS, JOB_BUSY, 1);

    /* (CHIP_STONEY and later: the earlier chips set VCE_VCPU_CNTL.CLK_EN instead: not ported) */
    WREG32_P(mmVCE_VCPU_CNTL, 1, ~0x200001);

    WREG32_FIELD(VCE_SOFT_RESET, ECPU_SOFT_RESET, 0);
    mdelay(100);

    r = firmware_loaded();

    /* clear BUSY flag */
    WREG32_FIELD(VCE_STATUS, JOB_BUSY, 0);

    if (!r) {
      /* VCE not responding, giving up */
      failure_ = read_diagnostics(registers_, config_.harvest_config);
      return Status::TimedOut;
    }
  }

  WREG32(mmGRBM_GFX_INDEX, mmGRBM_GFX_INDEX_DEFAULT);
  return Status::Ok;
}

AmdVce3::Status AmdVce3::start(const AmdVcnFirmware& firmware, const Memory& memory, const Config& config) {
  if (!firmware_usable(firmware)) return Status::BadFirmware;
  if ((config.harvest_config & (kHarvestVce0 | kHarvestVce1)) == (kHarvestVce0 | kHarvestVce1)) return Status::NoEngine;   // vce_v3_0_early_init: -ENOENT
  if (config.rings < 2 || config.rings > kMaxRings) return Status::BadMemory;
  auto fits = [](uint64_t gpu_address, const MmioRegion& window, uint64_t bytes) {
    return gpu_address != 0 && gpu_address % kPageSize == 0 && gpu_address <= UINT64_MAX - bytes && window.length() >= bytes && window.address() % 4 == 0;
  };
  if (!fits(memory.block_gpu_address, memory.block, kBlockBytes)) return Status::BadMemory;
  for (uint32_t r = 0; r < config.rings; ++r) {
    if (!fits(memory.ring_gpu_address[r], memory.ring[r], kRingBytes)) return Status::BadMemory;
    // The ranges, as the GPU sees them, must not overlap: the engine writes to the block and to the rings.
    if (memory.block_gpu_address < memory.ring_gpu_address[r] + kRingBytes && memory.ring_gpu_address[r] < memory.block_gpu_address + kBlockBytes) return Status::BadMemory;
    for (uint32_t q = 0; q < r; ++q)
      if (memory.ring_gpu_address[q] < memory.ring_gpu_address[r] + kRingBytes && memory.ring_gpu_address[r] < memory.ring_gpu_address[q] + kRingBytes) return Status::BadMemory;
  }

  // amdgpu_vce_resume: the microcode at the start of the block, everything after it cleared (the driver's buffer comes from the card's cleared memory).
  const ConstByteView ucode = firmware.ucode();
  uint64_t offset = 0;
  for (; offset + 4 <= ucode.size(); offset += 4)
    memory.block.write32(offset, static_cast<uint32_t>(ucode[offset]) | static_cast<uint32_t>(ucode[offset + 1]) << 8 | static_cast<uint32_t>(ucode[offset + 2]) << 16 |
                                     static_cast<uint32_t>(ucode[offset + 3]) << 24);
  if (offset < ucode.size()) {   // a microcode size that is not a multiple of four: the last word, padded with zeros
    uint32_t last = 0;
    for (uint64_t i = offset; i < ucode.size(); ++i) last |= static_cast<uint32_t>(ucode[i]) << (8 * (i - offset));
    memory.block.write32(offset, last);
    offset += 4;
  }
  for (; offset < kBlockBytes; offset += 4) memory.block.write32(offset, 0);
  for (uint32_t r = 0; r < config.rings; ++r)
    for (uint64_t o = 0; o < kRingBytes; o += 4) memory.ring[r].write32(o, 0);
  // Not in the driver, which writes through a mapping it flushes itself: what was just written through the card's memory BAR is buffered by the host data path, and the
  // engine reads it from the memory controller's side. The flush is vi_flush_hdp without a ring.
  WREG32(mmHDP_MEM_COHERENCY_FLUSH_CNTL, 1);
  RREG32(mmHDP_MEM_COHERENCY_FLUSH_CNTL);

  config_ = config;
  // vce_v3_0_hw_init, then the clock gating "ungate" that runs before the engine is used: the override first, the clocks of each instance next.
  override_vce_clock_gating(true);
  if (config_.medium_grain_clock_gating) set_clockgating_state(false);   // vce_v3_0_set_clockgating_state(AMD_CG_STATE_UNGATE)
  /* (not ported: amdgpu_asic_set_vce_clocks(adev, 10000, 10000), the clocks are the SMC's to set) */
  const Status status = run_start(memory);
  if (status != Status::Ok) {
    stop();   // the driver's hw_fini after a failed hw_init
    return status;
  }
  started_ = true;
  return Status::Ok;
}

// vce_v3_0_stop
void AmdVce3::stop() {
  int idx;

  for (idx = 0; idx < 2; ++idx) {
    if (config_.harvest_config & (1 << idx))
      continue;

    WREG32(mmGRBM_GFX_INDEX, GET_VCE_INSTANCE(idx));

    /* (CHIP_STONEY and later: the earlier chips clear VCE_VCPU_CNTL.CLK_EN instead: not ported) */
    WREG32_P(mmVCE_VCPU_CNTL, 0, ~0x200001);

    /* hold on ECPU */
    WREG32_FIELD(VCE_SOFT_RESET, ECPU_SOFT_RESET, 1);

    /* clear VCE STATUS */
    WREG32(mmVCE_STATUS, 0);
  }

  WREG32(mmGRBM_GFX_INDEX, mmGRBM_GFX_INDEX_DEFAULT);
  started_ = false;
}

}  // namespace hardware
