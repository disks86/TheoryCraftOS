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

// Ported from the Linux kernel's drivers/gpu/drm/amd/amdgpu/vcn_v1_0.c and amdgpu_vcn.c (Copyright 2016 Advanced Micro Devices, Inc.; MIT license). The
// register names and the order of the writes are the driver's: the macros below are the driver's own (SOC15 register access) so each function can be read side
// by side with its original. Differences from the driver are marked "not ported:".
#include "Hardware/VideoCodec/AMD/AmdVcn1.hpp"

namespace hardware {

namespace {
// UVD_BASE of the Vega10 register-base table, which Raven uses (vega10_ip_offset.h): the dword offset of each register segment in the register BAR.
constexpr uint32_t kUvdBase[5] = {0x00007800, 0x00007E00, 0, 0, 0};

// engine_status_constants of amdgpu_vcn.h
constexpr uint32_t UVD_PGFSM_STATUS__UVDM_UVDU_PWR_ON = 0x2AAAA0;
constexpr uint32_t UVD_PGFSM_CONFIG__UVDM_UVDU_PWR_ON = 0x00000002;
constexpr uint32_t UVD_STATUS__UVD_BUSY = 0x00000004;
constexpr uint32_t UVD_STATUS__IDLE = 0x2;
constexpr uint32_t UVD_POWER_STATUS__UVD_POWER_STATUS_TILES_OFF = 0x1;

// Two registers vcn_v1_0.c defines for itself (lines 43-46 there), not in the shared register headers.
constexpr uint32_t mmUVD_RBC_XX_IB_REG_CHECK_1_0 = 0x05ab;
constexpr uint32_t mmUVD_RBC_XX_IB_REG_CHECK_1_0_BASE_IDX = 1;
constexpr uint32_t mmUVD_REG_XX_MASK_1_0 = 0x05ac;
constexpr uint32_t mmUVD_REG_XX_MASK_1_0_BASE_IDX = 1;

// The register numbers, bases and field masks this file uses, generated from vcn_1_0_offset.h and vcn_1_0_sh_mask.h by scripts/gen_vcn1_regs.py.
#include "Hardware/VideoCodec/AMD/AmdVcn1Registers.inc"

constexpr uint32_t lower_32_bits(uint64_t v) { return static_cast<uint32_t>(v); }
constexpr uint32_t upper_32_bits(uint64_t v) { return static_cast<uint32_t>(v >> 32); }
constexpr uint64_t gpu_page_align(uint64_t v) { return (v + AmdVcn1::kPageSize - 1) & ~(AmdVcn1::kPageSize - 1); }
constexpr uint32_t order_base_2(uint64_t v) { uint32_t n = 0; while ((uint64_t{1} << n) < v) ++n; return n; }
}  // namespace

// The driver's register access macros, reduced to this class: UVD and VCN are the same register block (one base table), there is one instance.
#define SOC15_REG_OFFSET(ip, inst, reg) (kUvdBase[reg##_BASE_IDX] + (reg))
#define RREG32_SOC15(ip, inst, reg) rd(SOC15_REG_OFFSET(ip, inst, reg))
#define WREG32_SOC15(ip, inst, reg, value) wr(SOC15_REG_OFFSET(ip, inst, reg), (value))
#define WREG32_P(reg, val, mask) do { uint32_t t_ = rd(reg); t_ &= (mask); t_ |= ((val) & ~(mask)); wr(reg, t_); } while (0)
#define SOC15_WAIT_ON_RREG(ip, inst, reg, expected, mask) wait_on(SOC15_REG_OFFSET(ip, inst, reg), (expected), (mask))
#define REG_SET_FIELD(orig, reg, field, val) (((orig) & ~reg##__##field##_MASK) | (reg##__##field##_MASK & ((val) << reg##__##field##__SHIFT)))

bool AmdVcn1::wait_on(uint32_t dword, uint32_t expected, uint32_t mask) {
  // amdgpu_device_wait_on_rreg: poll for up to the driver's usec_timeout (100 ms). The VCN code ignores the result, and so does this.
  constexpr uint64_t kTimeoutUs = 100000;
  const uint64_t begin = clock_.now_us();
  do {
    if ((rd(dword) & mask) == expected) return true;
  } while (clock_.now_us() - begin < kTimeoutUs);
  return false;
}

void AmdVcn1::delay_ms(uint32_t ms) {
  const uint64_t begin = clock_.now_us();
  while (clock_.now_us() - begin < uint64_t{ms} * 1000) {}
}

uint64_t AmdVcn1::firmware_window_bytes(const AmdVcnFirmware& firmware) {
  return gpu_page_align(firmware.image().size() + 4);   // vcn_v1_0_mc_resume_spg_mode: AMDGPU_GPU_PAGE_ALIGN(fw->size + 4)
}

uint64_t AmdVcn1::block_bytes(const AmdVcnFirmware& firmware) {
  return firmware_window_bytes(firmware) + kStackBytes + kContextBytes + kSharedBytes;
}

void AmdVcn1::disable_static_power_gating() {
  uint32_t data = 0;

  if (config_.power_gating) {
    data = (1 << UVD_PGFSM_CONFIG__UVDM_PWR_CONFIG__SHIFT
          | 1 << UVD_PGFSM_CONFIG__UVDU_PWR_CONFIG__SHIFT
          | 2 << UVD_PGFSM_CONFIG__UVDF_PWR_CONFIG__SHIFT
          | 2 << UVD_PGFSM_CONFIG__UVDC_PWR_CONFIG__SHIFT
          | 2 << UVD_PGFSM_CONFIG__UVDB_PWR_CONFIG__SHIFT
          | 2 << UVD_PGFSM_CONFIG__UVDIL_PWR_CONFIG__SHIFT
          | 2 << UVD_PGFSM_CONFIG__UVDIR_PWR_CONFIG__SHIFT
          | 2 << UVD_PGFSM_CONFIG__UVDTD_PWR_CONFIG__SHIFT
          | 2 << UVD_PGFSM_CONFIG__UVDTE_PWR_CONFIG__SHIFT
          | 2 << UVD_PGFSM_CONFIG__UVDE_PWR_CONFIG__SHIFT
          | 2 << UVD_PGFSM_CONFIG__UVDW_PWR_CONFIG__SHIFT);

    WREG32_SOC15(VCN, 0, mmUVD_PGFSM_CONFIG, data);
    SOC15_WAIT_ON_RREG(VCN, 0, mmUVD_PGFSM_STATUS, UVD_PGFSM_STATUS__UVDM_UVDU_PWR_ON, 0xFFFFFF);
  } else {
    data = (1 << UVD_PGFSM_CONFIG__UVDM_PWR_CONFIG__SHIFT
          | 1 << UVD_PGFSM_CONFIG__UVDU_PWR_CONFIG__SHIFT
          | 1 << UVD_PGFSM_CONFIG__UVDF_PWR_CONFIG__SHIFT
          | 1 << UVD_PGFSM_CONFIG__UVDC_PWR_CONFIG__SHIFT
          | 1 << UVD_PGFSM_CONFIG__UVDB_PWR_CONFIG__SHIFT
          | 1 << UVD_PGFSM_CONFIG__UVDIL_PWR_CONFIG__SHIFT
          | 1 << UVD_PGFSM_CONFIG__UVDIR_PWR_CONFIG__SHIFT
          | 1 << UVD_PGFSM_CONFIG__UVDTD_PWR_CONFIG__SHIFT
          | 1 << UVD_PGFSM_CONFIG__UVDTE_PWR_CONFIG__SHIFT
          | 1 << UVD_PGFSM_CONFIG__UVDE_PWR_CONFIG__SHIFT
          | 1 << UVD_PGFSM_CONFIG__UVDW_PWR_CONFIG__SHIFT);
    WREG32_SOC15(VCN, 0, mmUVD_PGFSM_CONFIG, data);
    SOC15_WAIT_ON_RREG(VCN, 0, mmUVD_PGFSM_STATUS, 0, 0xFFFFFFFF);
  }

  /* polling UVD_PGFSM_STATUS to confirm UVDM_PWR_STATUS , UVDU_PWR_STATUS are 0 (power on) */

  data = RREG32_SOC15(VCN, 0, mmUVD_POWER_STATUS);
  data &= ~0x103;
  if (config_.power_gating)
    data |= UVD_PGFSM_CONFIG__UVDM_UVDU_PWR_ON | UVD_POWER_STATUS__UVD_PG_EN_MASK;

  WREG32_SOC15(VCN, 0, mmUVD_POWER_STATUS, data);
}

void AmdVcn1::enable_static_power_gating() {
  uint32_t data = 0;

  if (config_.power_gating) {
    /* Before power off, this indicator has to be turned on */
    data = RREG32_SOC15(VCN, 0, mmUVD_POWER_STATUS);
    data &= ~UVD_POWER_STATUS__UVD_POWER_STATUS_MASK;
    data |= UVD_POWER_STATUS__UVD_POWER_STATUS_TILES_OFF;
    WREG32_SOC15(VCN, 0, mmUVD_POWER_STATUS, data);

    data = (2 << UVD_PGFSM_CONFIG__UVDM_PWR_CONFIG__SHIFT
          | 2 << UVD_PGFSM_CONFIG__UVDU_PWR_CONFIG__SHIFT
          | 2 << UVD_PGFSM_CONFIG__UVDF_PWR_CONFIG__SHIFT
          | 2 << UVD_PGFSM_CONFIG__UVDC_PWR_CONFIG__SHIFT
          | 2 << UVD_PGFSM_CONFIG__UVDB_PWR_CONFIG__SHIFT
          | 2 << UVD_PGFSM_CONFIG__UVDIL_PWR_CONFIG__SHIFT
          | 2 << UVD_PGFSM_CONFIG__UVDIR_PWR_CONFIG__SHIFT
          | 2 << UVD_PGFSM_CONFIG__UVDTD_PWR_CONFIG__SHIFT
          | 2 << UVD_PGFSM_CONFIG__UVDTE_PWR_CONFIG__SHIFT
          | 2 << UVD_PGFSM_CONFIG__UVDE_PWR_CONFIG__SHIFT
          | 2 << UVD_PGFSM_CONFIG__UVDW_PWR_CONFIG__SHIFT);

    WREG32_SOC15(VCN, 0, mmUVD_PGFSM_CONFIG, data);

    data = (2 << UVD_PGFSM_STATUS__UVDM_PWR_STATUS__SHIFT
          | 2 << UVD_PGFSM_STATUS__UVDU_PWR_STATUS__SHIFT
          | 2 << UVD_PGFSM_STATUS__UVDF_PWR_STATUS__SHIFT
          | 2 << UVD_PGFSM_STATUS__UVDC_PWR_STATUS__SHIFT
          | 2 << UVD_PGFSM_STATUS__UVDB_PWR_STATUS__SHIFT
          | 2 << UVD_PGFSM_STATUS__UVDIL_PWR_STATUS__SHIFT
          | 2 << UVD_PGFSM_STATUS__UVDIR_PWR_STATUS__SHIFT
          | 2 << UVD_PGFSM_STATUS__UVDTD_PWR_STATUS__SHIFT
          | 2 << UVD_PGFSM_STATUS__UVDTE_PWR_STATUS__SHIFT
          | 2 << UVD_PGFSM_STATUS__UVDE_PWR_STATUS__SHIFT
          | 2 << UVD_PGFSM_STATUS__UVDW_PWR_STATUS__SHIFT);
    SOC15_WAIT_ON_RREG(VCN, 0, mmUVD_PGFSM_STATUS, data, 0xFFFFFFFF);
  }
}

void AmdVcn1::disable_clock_gating() {
  uint32_t data;

  /* JPEG disable CGC */
  data = RREG32_SOC15(VCN, 0, mmJPEG_CGC_CTRL);

  if (config_.clock_gating)
    data |= 1 << JPEG_CGC_CTRL__DYN_CLOCK_MODE__SHIFT;
  else
    data &= ~JPEG_CGC_CTRL__DYN_CLOCK_MODE_MASK;

  data |= 1 << JPEG_CGC_CTRL__CLK_GATE_DLY_TIMER__SHIFT;
  data |= 4 << JPEG_CGC_CTRL__CLK_OFF_DELAY__SHIFT;
  WREG32_SOC15(VCN, 0, mmJPEG_CGC_CTRL, data);

  data = RREG32_SOC15(VCN, 0, mmJPEG_CGC_GATE);
  data &= ~(JPEG_CGC_GATE__JPEG_MASK | JPEG_CGC_GATE__JPEG2_MASK);
  WREG32_SOC15(VCN, 0, mmJPEG_CGC_GATE, data);

  /* UVD disable CGC */
  data = RREG32_SOC15(VCN, 0, mmUVD_CGC_CTRL);
  if (config_.clock_gating)
    data |= 1 << UVD_CGC_CTRL__DYN_CLOCK_MODE__SHIFT;
  else
    data &= ~UVD_CGC_CTRL__DYN_CLOCK_MODE_MASK;

  data |= 1 << UVD_CGC_CTRL__CLK_GATE_DLY_TIMER__SHIFT;
  data |= 4 << UVD_CGC_CTRL__CLK_OFF_DELAY__SHIFT;
  WREG32_SOC15(VCN, 0, mmUVD_CGC_CTRL, data);

  data = RREG32_SOC15(VCN, 0, mmUVD_CGC_GATE);
  data &= ~(UVD_CGC_GATE__SYS_MASK
          | UVD_CGC_GATE__UDEC_MASK
          | UVD_CGC_GATE__MPEG2_MASK
          | UVD_CGC_GATE__REGS_MASK
          | UVD_CGC_GATE__RBC_MASK
          | UVD_CGC_GATE__LMI_MC_MASK
          | UVD_CGC_GATE__LMI_UMC_MASK
          | UVD_CGC_GATE__IDCT_MASK
          | UVD_CGC_GATE__MPRD_MASK
          | UVD_CGC_GATE__MPC_MASK
          | UVD_CGC_GATE__LBSI_MASK
          | UVD_CGC_GATE__LRBBM_MASK
          | UVD_CGC_GATE__UDEC_RE_MASK
          | UVD_CGC_GATE__UDEC_CM_MASK
          | UVD_CGC_GATE__UDEC_IT_MASK
          | UVD_CGC_GATE__UDEC_DB_MASK
          | UVD_CGC_GATE__UDEC_MP_MASK
          | UVD_CGC_GATE__WCB_MASK
          | UVD_CGC_GATE__VCPU_MASK
          | UVD_CGC_GATE__SCPU_MASK);
  WREG32_SOC15(VCN, 0, mmUVD_CGC_GATE, data);

  data = RREG32_SOC15(VCN, 0, mmUVD_CGC_CTRL);
  data &= ~(UVD_CGC_CTRL__UDEC_RE_MODE_MASK
          | UVD_CGC_CTRL__UDEC_CM_MODE_MASK
          | UVD_CGC_CTRL__UDEC_IT_MODE_MASK
          | UVD_CGC_CTRL__UDEC_DB_MODE_MASK
          | UVD_CGC_CTRL__UDEC_MP_MODE_MASK
          | UVD_CGC_CTRL__SYS_MODE_MASK
          | UVD_CGC_CTRL__UDEC_MODE_MASK
          | UVD_CGC_CTRL__MPEG2_MODE_MASK
          | UVD_CGC_CTRL__REGS_MODE_MASK
          | UVD_CGC_CTRL__RBC_MODE_MASK
          | UVD_CGC_CTRL__LMI_MC_MODE_MASK
          | UVD_CGC_CTRL__LMI_UMC_MODE_MASK
          | UVD_CGC_CTRL__IDCT_MODE_MASK
          | UVD_CGC_CTRL__MPRD_MODE_MASK
          | UVD_CGC_CTRL__MPC_MODE_MASK
          | UVD_CGC_CTRL__LBSI_MODE_MASK
          | UVD_CGC_CTRL__LRBBM_MODE_MASK
          | UVD_CGC_CTRL__WCB_MODE_MASK
          | UVD_CGC_CTRL__VCPU_MODE_MASK
          | UVD_CGC_CTRL__SCPU_MODE_MASK);
  WREG32_SOC15(VCN, 0, mmUVD_CGC_CTRL, data);

  /* turn on */
  data = RREG32_SOC15(VCN, 0, mmUVD_SUVD_CGC_GATE);
  data |= (UVD_SUVD_CGC_GATE__SRE_MASK
         | UVD_SUVD_CGC_GATE__SIT_MASK
         | UVD_SUVD_CGC_GATE__SMP_MASK
         | UVD_SUVD_CGC_GATE__SCM_MASK
         | UVD_SUVD_CGC_GATE__SDB_MASK
         | UVD_SUVD_CGC_GATE__SRE_H264_MASK
         | UVD_SUVD_CGC_GATE__SRE_HEVC_MASK
         | UVD_SUVD_CGC_GATE__SIT_H264_MASK
         | UVD_SUVD_CGC_GATE__SIT_HEVC_MASK
         | UVD_SUVD_CGC_GATE__SCM_H264_MASK
         | UVD_SUVD_CGC_GATE__SCM_HEVC_MASK
         | UVD_SUVD_CGC_GATE__SDB_H264_MASK
         | UVD_SUVD_CGC_GATE__SDB_HEVC_MASK
         | UVD_SUVD_CGC_GATE__SCLR_MASK
         | UVD_SUVD_CGC_GATE__UVD_SC_MASK
         | UVD_SUVD_CGC_GATE__ENT_MASK
         | UVD_SUVD_CGC_GATE__SIT_HEVC_DEC_MASK
         | UVD_SUVD_CGC_GATE__SIT_HEVC_ENC_MASK
         | UVD_SUVD_CGC_GATE__SITE_MASK
         | UVD_SUVD_CGC_GATE__SRE_VP9_MASK
         | UVD_SUVD_CGC_GATE__SCM_VP9_MASK
         | UVD_SUVD_CGC_GATE__SIT_VP9_DEC_MASK
         | UVD_SUVD_CGC_GATE__SDB_VP9_MASK
         | UVD_SUVD_CGC_GATE__IME_HEVC_MASK);
  WREG32_SOC15(VCN, 0, mmUVD_SUVD_CGC_GATE, data);

  data = RREG32_SOC15(VCN, 0, mmUVD_SUVD_CGC_CTRL);
  data &= ~(UVD_SUVD_CGC_CTRL__SRE_MODE_MASK
          | UVD_SUVD_CGC_CTRL__SIT_MODE_MASK
          | UVD_SUVD_CGC_CTRL__SMP_MODE_MASK
          | UVD_SUVD_CGC_CTRL__SCM_MODE_MASK
          | UVD_SUVD_CGC_CTRL__SDB_MODE_MASK
          | UVD_SUVD_CGC_CTRL__SCLR_MODE_MASK
          | UVD_SUVD_CGC_CTRL__UVD_SC_MODE_MASK
          | UVD_SUVD_CGC_CTRL__ENT_MODE_MASK
          | UVD_SUVD_CGC_CTRL__IME_MODE_MASK
          | UVD_SUVD_CGC_CTRL__SITE_MODE_MASK);
  WREG32_SOC15(VCN, 0, mmUVD_SUVD_CGC_CTRL, data);
}

void AmdVcn1::enable_clock_gating() {
  uint32_t data = 0;

  /* enable JPEG CGC */
  data = RREG32_SOC15(VCN, 0, mmJPEG_CGC_CTRL);
  if (config_.clock_gating)
    data |= 1 << JPEG_CGC_CTRL__DYN_CLOCK_MODE__SHIFT;
  else
    data |= 0 << JPEG_CGC_CTRL__DYN_CLOCK_MODE__SHIFT;
  data |= 1 << JPEG_CGC_CTRL__CLK_GATE_DLY_TIMER__SHIFT;
  data |= 4 << JPEG_CGC_CTRL__CLK_OFF_DELAY__SHIFT;
  WREG32_SOC15(VCN, 0, mmJPEG_CGC_CTRL, data);

  data = RREG32_SOC15(VCN, 0, mmJPEG_CGC_GATE);
  data |= (JPEG_CGC_GATE__JPEG_MASK | JPEG_CGC_GATE__JPEG2_MASK);
  WREG32_SOC15(VCN, 0, mmJPEG_CGC_GATE, data);

  /* enable UVD CGC */
  data = RREG32_SOC15(VCN, 0, mmUVD_CGC_CTRL);
  if (config_.clock_gating)
    data |= 1 << UVD_CGC_CTRL__DYN_CLOCK_MODE__SHIFT;
  else
    data |= 0 << UVD_CGC_CTRL__DYN_CLOCK_MODE__SHIFT;
  data |= 1 << UVD_CGC_CTRL__CLK_GATE_DLY_TIMER__SHIFT;
  data |= 4 << UVD_CGC_CTRL__CLK_OFF_DELAY__SHIFT;
  WREG32_SOC15(VCN, 0, mmUVD_CGC_CTRL, data);

  data = RREG32_SOC15(VCN, 0, mmUVD_CGC_CTRL);
  data |= (UVD_CGC_CTRL__UDEC_RE_MODE_MASK
         | UVD_CGC_CTRL__UDEC_CM_MODE_MASK
         | UVD_CGC_CTRL__UDEC_IT_MODE_MASK
         | UVD_CGC_CTRL__UDEC_DB_MODE_MASK
         | UVD_CGC_CTRL__UDEC_MP_MODE_MASK
         | UVD_CGC_CTRL__SYS_MODE_MASK
         | UVD_CGC_CTRL__UDEC_MODE_MASK
         | UVD_CGC_CTRL__MPEG2_MODE_MASK
         | UVD_CGC_CTRL__REGS_MODE_MASK
         | UVD_CGC_CTRL__RBC_MODE_MASK
         | UVD_CGC_CTRL__LMI_MC_MODE_MASK
         | UVD_CGC_CTRL__LMI_UMC_MODE_MASK
         | UVD_CGC_CTRL__IDCT_MODE_MASK
         | UVD_CGC_CTRL__MPRD_MODE_MASK
         | UVD_CGC_CTRL__MPC_MODE_MASK
         | UVD_CGC_CTRL__LBSI_MODE_MASK
         | UVD_CGC_CTRL__LRBBM_MODE_MASK
         | UVD_CGC_CTRL__WCB_MODE_MASK
         | UVD_CGC_CTRL__VCPU_MODE_MASK
         | UVD_CGC_CTRL__SCPU_MODE_MASK);
  WREG32_SOC15(VCN, 0, mmUVD_CGC_CTRL, data);

  data = RREG32_SOC15(VCN, 0, mmUVD_SUVD_CGC_CTRL);
  data |= (UVD_SUVD_CGC_CTRL__SRE_MODE_MASK
         | UVD_SUVD_CGC_CTRL__SIT_MODE_MASK
         | UVD_SUVD_CGC_CTRL__SMP_MODE_MASK
         | UVD_SUVD_CGC_CTRL__SCM_MODE_MASK
         | UVD_SUVD_CGC_CTRL__SDB_MODE_MASK
         | UVD_SUVD_CGC_CTRL__SCLR_MODE_MASK
         | UVD_SUVD_CGC_CTRL__UVD_SC_MODE_MASK
         | UVD_SUVD_CGC_CTRL__ENT_MODE_MASK
         | UVD_SUVD_CGC_CTRL__IME_MODE_MASK
         | UVD_SUVD_CGC_CTRL__SITE_MODE_MASK);
  WREG32_SOC15(VCN, 0, mmUVD_SUVD_CGC_CTRL, data);
}

// vcn_v1_0_mc_resume_spg_mode: the microcode was loaded directly (not by the PSP), so cache window 0 is at the start of the block.
void AmdVcn1::mc_resume(const AmdVcnFirmware& firmware, const Memory& memory) {
  const uint32_t size = static_cast<uint32_t>(firmware_window_bytes(firmware));
  const uint64_t gpu_addr = memory.block_gpu_address;
  uint32_t offset;

  /* cache window 0: fw */
  WREG32_SOC15(UVD, 0, mmUVD_LMI_VCPU_CACHE_64BIT_BAR_LOW, lower_32_bits(gpu_addr));
  WREG32_SOC15(UVD, 0, mmUVD_LMI_VCPU_CACHE_64BIT_BAR_HIGH, upper_32_bits(gpu_addr));
  offset = size;
  WREG32_SOC15(UVD, 0, mmUVD_VCPU_CACHE_OFFSET0, kFirmwareOffset >> 3);

  WREG32_SOC15(UVD, 0, mmUVD_VCPU_CACHE_SIZE0, size);

  /* cache window 1: stack */
  WREG32_SOC15(UVD, 0, mmUVD_LMI_VCPU_CACHE1_64BIT_BAR_LOW, lower_32_bits(gpu_addr + offset));
  WREG32_SOC15(UVD, 0, mmUVD_LMI_VCPU_CACHE1_64BIT_BAR_HIGH, upper_32_bits(gpu_addr + offset));
  WREG32_SOC15(UVD, 0, mmUVD_VCPU_CACHE_OFFSET1, 0);
  WREG32_SOC15(UVD, 0, mmUVD_VCPU_CACHE_SIZE1, kStackBytes);

  /* cache window 2: context */
  WREG32_SOC15(UVD, 0, mmUVD_LMI_VCPU_CACHE2_64BIT_BAR_LOW, lower_32_bits(gpu_addr + offset + kStackBytes));
  WREG32_SOC15(UVD, 0, mmUVD_LMI_VCPU_CACHE2_64BIT_BAR_HIGH, upper_32_bits(gpu_addr + offset + kStackBytes));
  WREG32_SOC15(UVD, 0, mmUVD_VCPU_CACHE_OFFSET2, 0);
  WREG32_SOC15(UVD, 0, mmUVD_VCPU_CACHE_SIZE2, kContextBytes);

  WREG32_SOC15(UVD, 0, mmUVD_UDEC_ADDR_CONFIG, config_.gb_addr_config);
  WREG32_SOC15(UVD, 0, mmUVD_UDEC_DB_ADDR_CONFIG, config_.gb_addr_config);
  WREG32_SOC15(UVD, 0, mmUVD_UDEC_DBW_ADDR_CONFIG, config_.gb_addr_config);
  WREG32_SOC15(UVD, 0, mmUVD_UDEC_DBW_UV_ADDR_CONFIG, config_.gb_addr_config);
  WREG32_SOC15(UVD, 0, mmUVD_MIF_CURR_ADDR_CONFIG, config_.gb_addr_config);
  WREG32_SOC15(UVD, 0, mmUVD_MIF_CURR_UV_ADDR_CONFIG, config_.gb_addr_config);
  WREG32_SOC15(UVD, 0, mmUVD_MIF_RECON1_ADDR_CONFIG, config_.gb_addr_config);
  WREG32_SOC15(UVD, 0, mmUVD_MIF_RECON1_UV_ADDR_CONFIG, config_.gb_addr_config);
  WREG32_SOC15(UVD, 0, mmUVD_MIF_REF_ADDR_CONFIG, config_.gb_addr_config);
  WREG32_SOC15(UVD, 0, mmUVD_MIF_REF_UV_ADDR_CONFIG, config_.gb_addr_config);
  WREG32_SOC15(UVD, 0, mmUVD_JPEG_ADDR_CONFIG, config_.gb_addr_config);
  WREG32_SOC15(UVD, 0, mmUVD_JPEG_UV_ADDR_CONFIG, config_.gb_addr_config);
}

// vcn_v1_0_start_spg_mode
AmdVcn1::Status AmdVcn1::start_spg_mode(const AmdVcnFirmware& firmware, const Memory& memory) {
  uint32_t rb_bufsz, tmp;
  uint32_t lmi_swap_cntl;
  int i, j, r;

  /* disable byte swapping */
  lmi_swap_cntl = 0;

  disable_static_power_gating();

  tmp = RREG32_SOC15(UVD, 0, mmUVD_STATUS) | UVD_STATUS__UVD_BUSY;
  WREG32_SOC15(UVD, 0, mmUVD_STATUS, tmp);

  /* disable clock gating */
  disable_clock_gating();

  /* disable interupt */
  WREG32_P(SOC15_REG_OFFSET(UVD, 0, mmUVD_MASTINT_EN), 0, ~UVD_MASTINT_EN__VCPU_EN_MASK);

  /* initialize VCN memory controller */
  tmp = RREG32_SOC15(UVD, 0, mmUVD_LMI_CTRL);
  WREG32_SOC15(UVD, 0, mmUVD_LMI_CTRL, tmp |
      UVD_LMI_CTRL__WRITE_CLEAN_TIMER_EN_MASK |
      UVD_LMI_CTRL__MASK_MC_URGENT_MASK |
      UVD_LMI_CTRL__DATA_COHERENCY_EN_MASK |
      UVD_LMI_CTRL__VCPU_DATA_COHERENCY_EN_MASK);

  WREG32_SOC15(UVD, 0, mmUVD_LMI_SWAP_CNTL, lmi_swap_cntl);

  tmp = RREG32_SOC15(UVD, 0, mmUVD_MPC_CNTL);
  tmp &= ~UVD_MPC_CNTL__REPLACEMENT_MODE_MASK;
  tmp |= 0x2 << UVD_MPC_CNTL__REPLACEMENT_MODE__SHIFT;
  WREG32_SOC15(UVD, 0, mmUVD_MPC_CNTL, tmp);

  WREG32_SOC15(UVD, 0, mmUVD_MPC_SET_MUXA0,
      ((0x1 << UVD_MPC_SET_MUXA0__VARA_1__SHIFT) |
       (0x2 << UVD_MPC_SET_MUXA0__VARA_2__SHIFT) |
       (0x3 << UVD_MPC_SET_MUXA0__VARA_3__SHIFT) |
       (0x4 << UVD_MPC_SET_MUXA0__VARA_4__SHIFT)));

  WREG32_SOC15(UVD, 0, mmUVD_MPC_SET_MUXB0,
      ((0x1 << UVD_MPC_SET_MUXB0__VARB_1__SHIFT) |
       (0x2 << UVD_MPC_SET_MUXB0__VARB_2__SHIFT) |
       (0x3 << UVD_MPC_SET_MUXB0__VARB_3__SHIFT) |
       (0x4 << UVD_MPC_SET_MUXB0__VARB_4__SHIFT)));

  WREG32_SOC15(UVD, 0, mmUVD_MPC_SET_MUX,
      ((0x0 << UVD_MPC_SET_MUX__SET_0__SHIFT) |
       (0x1 << UVD_MPC_SET_MUX__SET_1__SHIFT) |
       (0x2 << UVD_MPC_SET_MUX__SET_2__SHIFT)));

  mc_resume(firmware, memory);

  WREG32_SOC15(UVD, 0, mmUVD_REG_XX_MASK_1_0, 0x10);
  WREG32_SOC15(UVD, 0, mmUVD_RBC_XX_IB_REG_CHECK_1_0,
      RREG32_SOC15(UVD, 0, mmUVD_RBC_XX_IB_REG_CHECK_1_0) | 0x3);

  /* enable VCPU clock */
  WREG32_SOC15(UVD, 0, mmUVD_VCPU_CNTL, UVD_VCPU_CNTL__CLK_EN_MASK);

  /* boot up the VCPU */
  WREG32_P(SOC15_REG_OFFSET(UVD, 0, mmUVD_SOFT_RESET), 0, ~UVD_SOFT_RESET__VCPU_SOFT_RESET_MASK);

  /* enable UMC */
  WREG32_P(SOC15_REG_OFFSET(UVD, 0, mmUVD_LMI_CTRL2), 0, ~UVD_LMI_CTRL2__STALL_ARB_UMC_MASK);

  tmp = RREG32_SOC15(UVD, 0, mmUVD_SOFT_RESET);
  tmp &= ~UVD_SOFT_RESET__LMI_SOFT_RESET_MASK;
  tmp &= ~UVD_SOFT_RESET__LMI_UMC_SOFT_RESET_MASK;
  WREG32_SOC15(UVD, 0, mmUVD_SOFT_RESET, tmp);

  for (i = 0; i < 10; ++i) {
    uint32_t status;

    for (j = 0; j < 100; ++j) {
      status = RREG32_SOC15(UVD, 0, mmUVD_STATUS);
      if (status & UVD_STATUS__IDLE)
        break;
      delay_ms(10);
    }
    r = 0;
    if (status & UVD_STATUS__IDLE)
      break;

    /* VCN decode not responding, trying to reset the VCPU */
    WREG32_P(SOC15_REG_OFFSET(UVD, 0, mmUVD_SOFT_RESET),
        UVD_SOFT_RESET__VCPU_SOFT_RESET_MASK,
        ~UVD_SOFT_RESET__VCPU_SOFT_RESET_MASK);
    delay_ms(10);
    WREG32_P(SOC15_REG_OFFSET(UVD, 0, mmUVD_SOFT_RESET), 0,
        ~UVD_SOFT_RESET__VCPU_SOFT_RESET_MASK);
    delay_ms(10);
    r = -1;
  }

  if (r)
    return Status::TimedOut;   /* VCN decode not responding, giving up */

  /* enable master interrupt */
  WREG32_P(SOC15_REG_OFFSET(UVD, 0, mmUVD_MASTINT_EN),
      UVD_MASTINT_EN__VCPU_EN_MASK, ~UVD_MASTINT_EN__VCPU_EN_MASK);

  /* enable system interrupt for JRBC, TODO: move to set interrupt*/
  WREG32_P(SOC15_REG_OFFSET(UVD, 0, mmUVD_SYS_INT_EN),
      UVD_SYS_INT_EN__UVD_JRBC_EN_MASK,
      ~UVD_SYS_INT_EN__UVD_JRBC_EN_MASK);

  /* clear the busy bit of UVD_STATUS */
  tmp = RREG32_SOC15(UVD, 0, mmUVD_STATUS) & ~UVD_STATUS__UVD_BUSY;
  WREG32_SOC15(UVD, 0, mmUVD_STATUS, tmp);

  /* The three rings (the driver's ring_dec and ring_enc[0..1]): the decode ring's registers, then the two encode rings'. */
  const uint64_t dec_gpu_addr = memory.rings_gpu_address;
  const uint64_t enc0_gpu_addr = memory.rings_gpu_address + kRingBytes;
  const uint64_t enc1_gpu_addr = memory.rings_gpu_address + 2 * kRingBytes;
  const uint32_t ring_size = static_cast<uint32_t>(kRingBytes);
  uint32_t wptr;

  /* force RBC into idle state */
  rb_bufsz = order_base_2(ring_size);
  tmp = REG_SET_FIELD(0, UVD_RBC_RB_CNTL, RB_BUFSZ, rb_bufsz);
  tmp = REG_SET_FIELD(tmp, UVD_RBC_RB_CNTL, RB_BLKSZ, 1);
  tmp = REG_SET_FIELD(tmp, UVD_RBC_RB_CNTL, RB_NO_FETCH, 1);
  tmp = REG_SET_FIELD(tmp, UVD_RBC_RB_CNTL, RB_NO_UPDATE, 1);
  tmp = REG_SET_FIELD(tmp, UVD_RBC_RB_CNTL, RB_RPTR_WR_EN, 1);
  WREG32_SOC15(UVD, 0, mmUVD_RBC_RB_CNTL, tmp);

  /* set the write pointer delay */
  WREG32_SOC15(UVD, 0, mmUVD_RBC_RB_WPTR_CNTL, 0);

  /* set the wb address */
  WREG32_SOC15(UVD, 0, mmUVD_RBC_RB_RPTR_ADDR, (upper_32_bits(dec_gpu_addr) >> 2));

  /* program the RB_BASE for ring buffer */
  WREG32_SOC15(UVD, 0, mmUVD_LMI_RBC_RB_64BIT_BAR_LOW, lower_32_bits(dec_gpu_addr));
  WREG32_SOC15(UVD, 0, mmUVD_LMI_RBC_RB_64BIT_BAR_HIGH, upper_32_bits(dec_gpu_addr));

  /* Initialize the ring buffer's read and write pointers */
  WREG32_SOC15(UVD, 0, mmUVD_RBC_RB_RPTR, 0);

  WREG32_SOC15(UVD, 0, mmUVD_SCRATCH2, 0);

  wptr = RREG32_SOC15(UVD, 0, mmUVD_RBC_RB_RPTR);
  WREG32_SOC15(UVD, 0, mmUVD_RBC_RB_WPTR, lower_32_bits(wptr));

  WREG32_P(SOC15_REG_OFFSET(UVD, 0, mmUVD_RBC_RB_CNTL), 0, ~UVD_RBC_RB_CNTL__RB_NO_FETCH_MASK);

  /* ring_enc[0] (a fresh ring: wptr 0) */
  WREG32_SOC15(UVD, 0, mmUVD_RB_RPTR, 0);
  WREG32_SOC15(UVD, 0, mmUVD_RB_WPTR, 0);
  WREG32_SOC15(UVD, 0, mmUVD_RB_BASE_LO, lower_32_bits(enc0_gpu_addr));
  WREG32_SOC15(UVD, 0, mmUVD_RB_BASE_HI, upper_32_bits(enc0_gpu_addr));
  WREG32_SOC15(UVD, 0, mmUVD_RB_SIZE, ring_size / 4);

  /* ring_enc[1] */
  WREG32_SOC15(UVD, 0, mmUVD_RB_RPTR2, 0);
  WREG32_SOC15(UVD, 0, mmUVD_RB_WPTR2, 0);
  WREG32_SOC15(UVD, 0, mmUVD_RB_BASE_LO2, lower_32_bits(enc1_gpu_addr));
  WREG32_SOC15(UVD, 0, mmUVD_RB_BASE_HI2, upper_32_bits(enc1_gpu_addr));
  WREG32_SOC15(UVD, 0, mmUVD_RB_SIZE2, ring_size / 4);

  /* not ported: jpeg_v1_0_start(adev, 0) - the JPEG ring is not used */

  /* Keeping one read-back to ensure all register writes are done,
   * otherwise it may introduce race conditions.
   */
  RREG32_SOC15(UVD, 0, mmUVD_STATUS);

  return Status::Ok;
}

// amdgpu_vcn_resume (direct load): the microcode at the start of the block, zeros after it; then vcn_v1_0_start.
AmdVcn1::Status AmdVcn1::start(const AmdVcnFirmware& firmware, const Memory& memory, const Config& config) {
  if (!firmware.loaded() || firmware.header().ip_version_major != 1) return Status::BadFirmware;
  const uint64_t block = block_bytes(firmware);
  auto fits = [](uint64_t gpu_address, const MmioRegion& window, uint64_t bytes) {
    return gpu_address != 0 && gpu_address % kPageSize == 0 && gpu_address <= UINT64_MAX - bytes && window.length() >= bytes && window.address() % 4 == 0;
  };
  if (!fits(memory.block_gpu_address, memory.block, block) || !fits(memory.rings_gpu_address, memory.rings, kRingsTotalBytes)) return Status::BadMemory;
  // The two ranges, as the GPU sees them, must not overlap: the engine writes to both.
  if (memory.block_gpu_address < memory.rings_gpu_address + kRingsTotalBytes && memory.rings_gpu_address < memory.block_gpu_address + block) return Status::BadMemory;

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
  for (; offset < block; offset += 4) memory.block.write32(offset, 0);
  for (uint64_t o = 0; o < kRingsTotalBytes; o += 4) memory.rings.write32(o, 0);

  config_ = config;
  const Status status = start_spg_mode(firmware, memory);
  if (status != Status::Ok) {
    stop();   // the driver's hw_fini after a failed hw_init: engine back in reset, clocks and power gated again
    return status;
  }
  started_ = true;
  return Status::Ok;
}

// vcn_v1_0_stop_spg_mode
void AmdVcn1::stop() {
  uint32_t tmp;

  SOC15_WAIT_ON_RREG(UVD, 0, mmUVD_STATUS, UVD_STATUS__IDLE, 0x7);

  tmp = UVD_LMI_STATUS__VCPU_LMI_WRITE_CLEAN_MASK |
        UVD_LMI_STATUS__READ_CLEAN_MASK |
        UVD_LMI_STATUS__WRITE_CLEAN_MASK |
        UVD_LMI_STATUS__WRITE_CLEAN_RAW_MASK;
  SOC15_WAIT_ON_RREG(UVD, 0, mmUVD_LMI_STATUS, tmp, tmp);

  /* stall UMC channel */
  WREG32_P(SOC15_REG_OFFSET(UVD, 0, mmUVD_LMI_CTRL2),
      UVD_LMI_CTRL2__STALL_ARB_UMC_MASK,
      ~UVD_LMI_CTRL2__STALL_ARB_UMC_MASK);

  tmp = UVD_LMI_STATUS__UMC_READ_CLEAN_RAW_MASK |
        UVD_LMI_STATUS__UMC_WRITE_CLEAN_RAW_MASK;
  SOC15_WAIT_ON_RREG(UVD, 0, mmUVD_LMI_STATUS, tmp, tmp);

  /* disable VCPU clock */
  WREG32_P(SOC15_REG_OFFSET(UVD, 0, mmUVD_VCPU_CNTL), 0, ~UVD_VCPU_CNTL__CLK_EN_MASK);

  /* reset LMI UMC/LMI */
  WREG32_P(SOC15_REG_OFFSET(UVD, 0, mmUVD_SOFT_RESET),
      UVD_SOFT_RESET__LMI_UMC_SOFT_RESET_MASK,
      ~UVD_SOFT_RESET__LMI_UMC_SOFT_RESET_MASK);

  WREG32_P(SOC15_REG_OFFSET(UVD, 0, mmUVD_SOFT_RESET),
      UVD_SOFT_RESET__LMI_SOFT_RESET_MASK,
      ~UVD_SOFT_RESET__LMI_SOFT_RESET_MASK);

  /* put VCPU into reset */
  WREG32_P(SOC15_REG_OFFSET(UVD, 0, mmUVD_SOFT_RESET),
      UVD_SOFT_RESET__VCPU_SOFT_RESET_MASK,
      ~UVD_SOFT_RESET__VCPU_SOFT_RESET_MASK);

  WREG32_SOC15(UVD, 0, mmUVD_STATUS, 0);

  enable_clock_gating();
  enable_static_power_gating();

  /* Keeping one read-back to ensure all register writes are done,
   * otherwise it may introduce race conditions.
   */
  RREG32_SOC15(UVD, 0, mmUVD_STATUS);
  started_ = false;
}

#undef SOC15_REG_OFFSET
#undef RREG32_SOC15
#undef WREG32_SOC15
#undef WREG32_P
#undef SOC15_WAIT_ON_RREG
#undef REG_SET_FIELD

}  // namespace hardware
