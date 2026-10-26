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

// Ported from the Linux kernel's drivers/gpu/drm/amd/amdgpu/uvd_v6_0.c and amdgpu_uvd.c (Copyright 2014 Advanced Micro Devices, Inc.; MIT license). The register
// names and the order of the writes are the driver's: the macros below are the driver's own so each function can be read side by side with its original.
// Differences from the driver are marked "not ported:".
#include "Hardware/VideoCodec/AMD/AmdUvd6.hpp"

namespace hardware {

namespace {
// The register numbers and field masks this file uses, generated from uvd_6_0_d.h, uvd_6_0_sh_mask.h, oss_3_0_d.h, oss_3_0_sh_mask.h and bif_5_0_d.h by scripts/gen_vcn1_regs.py --vi.
#include "Hardware/VideoCodec/AMD/AmdUvd6Registers.inc"

constexpr uint32_t lower_32_bits(uint64_t v) { return static_cast<uint32_t>(v); }
constexpr uint32_t upper_32_bits(uint64_t v) { return static_cast<uint32_t>(v >> 32); }
constexpr uint64_t gpu_page_align(uint64_t v) { return (v + AmdUvd6::kPageSize - 1) & ~(AmdUvd6::kPageSize - 1); }
constexpr uint32_t order_base_2(uint64_t v) { uint32_t n = 0; while ((uint64_t{1} << n) < v) ++n; return n; }
constexpr uint32_t FW_1_80_MINOR = 0x50;
}  // namespace

#define RREG32(reg) rd(reg)
#define WREG32(reg, value) wr(reg, (value))
#define WREG32_P(reg, val, mask) do { uint32_t t_ = rd(reg); t_ &= (mask); t_ |= ((val) & ~(mask)); wr(reg, t_); } while (0)
#define REG_FIELD_SHIFT(reg, field) reg##__##field##__SHIFT
#define REG_FIELD_MASK(reg, field) reg##__##field##_MASK
#define WREG32_FIELD(reg, field, val) wr(mm##reg, (rd(mm##reg) & ~REG_FIELD_MASK(reg, field)) | ((val) << REG_FIELD_SHIFT(reg, field)))
#define REG_SET_FIELD(orig, reg, field, val) (((orig) & ~reg##__##field##_MASK) | (reg##__##field##_MASK & ((val) << reg##__##field##__SHIFT)))
#define RREG32_UVD_CTX(reg) read_ctx(reg)
#define WREG32_UVD_CTX(reg, v) write_ctx(reg, (v))
#define mdelay(ms) delay_ms(ms)

const char* amd_uvd6_firmware_name(uint16_t pci_device_id) {
  // amdgpu_uvd.c: CHIP_VEGAM -> "amdgpu/vegam_uvd.bin"; amdgpu_drv.c lists 0x694C, 0x694E and 0x694F as CHIP_VEGAM.
  return pci_device_id == 0x694C || pci_device_id == 0x694E || pci_device_id == 0x694F ? "vegam_uvd.bin" : nullptr;
}

void AmdUvd6::delay_ms(uint32_t ms) {
  const uint64_t begin = clock_.now_us();
  while (clock_.now_us() - begin < uint64_t{ms} * 1000) {}
}

uint32_t AmdUvd6::max_handles_for(const AmdVcnFirmware& firmware) {
  // amdgpu_uvd_sw_init: the firmware with 40 sessions is 1.80; every later version has them too.
  const uint32_t version = firmware.header().ucode_version;
  const uint32_t version_major = (version >> 24) & 0xff, version_minor = (version >> 8) & 0xff;
  return version_major > 0x01 || (version_major == 0x01 && version_minor >= FW_1_80_MINOR) ? kMaxHandles : kDefaultHandles;
}

uint64_t AmdUvd6::firmware_window_bytes(const AmdVcnFirmware& firmware) {
  return gpu_page_align(uint64_t{firmware.header().ucode_size_bytes} + 8);   // AMDGPU_GPU_PAGE_ALIGN(ucode_size_bytes + 8)
}

uint64_t AmdUvd6::block_bytes(const AmdVcnFirmware& firmware, uint32_t max_handles) {
  return firmware_window_bytes(firmware) + kStackBytes + kHeapBytes + uint64_t{kSessionBytes} * max_handles;   // amdgpu_uvd_sw_init: bo_size
}

// RREG32_UVD_CTX / WREG32_UVD_CTX (vi.c): the engine's context registers are reached through an index and a data register.
uint32_t AmdUvd6::read_ctx(uint32_t reg) {
  WREG32(mmUVD_CTX_INDEX, reg & 0x1ff);
  return RREG32(mmUVD_CTX_DATA);
}

void AmdUvd6::write_ctx(uint32_t reg, uint32_t value) {
  WREG32(mmUVD_CTX_INDEX, reg & 0x1ff);
  WREG32(mmUVD_CTX_DATA, value);
}

// uvd_v6_0_enable_mgcg
void AmdUvd6::enable_mgcg(bool enable) {
  uint32_t orig, data;

  if (enable && config_.medium_grain_clock_gating) {
    data = RREG32_UVD_CTX(ixUVD_CGC_MEM_CTRL);
    data |= 0xfff;
    WREG32_UVD_CTX(ixUVD_CGC_MEM_CTRL, data);

    orig = data = RREG32(mmUVD_CGC_CTRL);
    data |= UVD_CGC_CTRL__DYN_CLOCK_MODE_MASK;
    if (orig != data)
      WREG32(mmUVD_CGC_CTRL, data);
  } else {
    data = RREG32_UVD_CTX(ixUVD_CGC_MEM_CTRL);
    data &= ~0xfff;
    WREG32_UVD_CTX(ixUVD_CGC_MEM_CTRL, data);

    orig = data = RREG32(mmUVD_CGC_CTRL);
    data &= ~UVD_CGC_CTRL__DYN_CLOCK_MODE_MASK;
    if (orig != data)
      WREG32(mmUVD_CGC_CTRL, data);
  }
}

// uvd_v6_0_enable_clock_gating
void AmdUvd6::enable_clock_gating(bool enable) {
  uint32_t data1, data3;

  data1 = RREG32(mmUVD_SUVD_CGC_GATE);
  data3 = RREG32(mmUVD_CGC_GATE);

  data1 |= UVD_SUVD_CGC_GATE__SRE_MASK |
	     UVD_SUVD_CGC_GATE__SIT_MASK |
	     UVD_SUVD_CGC_GATE__SMP_MASK |
	     UVD_SUVD_CGC_GATE__SCM_MASK |
	     UVD_SUVD_CGC_GATE__SDB_MASK |
	     UVD_SUVD_CGC_GATE__SRE_H264_MASK |
	     UVD_SUVD_CGC_GATE__SRE_HEVC_MASK |
	     UVD_SUVD_CGC_GATE__SIT_H264_MASK |
	     UVD_SUVD_CGC_GATE__SIT_HEVC_MASK |
	     UVD_SUVD_CGC_GATE__SCM_H264_MASK |
	     UVD_SUVD_CGC_GATE__SCM_HEVC_MASK |
	     UVD_SUVD_CGC_GATE__SDB_H264_MASK |
	     UVD_SUVD_CGC_GATE__SDB_HEVC_MASK;

  if (enable) {
    data3 |= (UVD_CGC_GATE__SYS_MASK       |
	    UVD_CGC_GATE__UDEC_MASK      |
	    UVD_CGC_GATE__MPEG2_MASK     |
	    UVD_CGC_GATE__RBC_MASK       |
	    UVD_CGC_GATE__LMI_MC_MASK    |
	    UVD_CGC_GATE__LMI_UMC_MASK   |
	    UVD_CGC_GATE__IDCT_MASK      |
	    UVD_CGC_GATE__MPRD_MASK      |
	    UVD_CGC_GATE__MPC_MASK       |
	    UVD_CGC_GATE__LBSI_MASK      |
	    UVD_CGC_GATE__LRBBM_MASK     |
	    UVD_CGC_GATE__UDEC_RE_MASK   |
	    UVD_CGC_GATE__UDEC_CM_MASK   |
	    UVD_CGC_GATE__UDEC_IT_MASK   |
	    UVD_CGC_GATE__UDEC_DB_MASK   |
	    UVD_CGC_GATE__UDEC_MP_MASK   |
	    UVD_CGC_GATE__WCB_MASK       |
	    UVD_CGC_GATE__JPEG_MASK      |
	    UVD_CGC_GATE__SCPU_MASK      |
	    UVD_CGC_GATE__JPEG2_MASK);
    /* only in pg enabled, we can gate clock to vcpu*/
    if (config_.power_gating)
      data3 |= UVD_CGC_GATE__VCPU_MASK;

    data3 &= ~UVD_CGC_GATE__REGS_MASK;
  } else {
    data3 = 0;
  }

  WREG32(mmUVD_SUVD_CGC_GATE, data1);
  WREG32(mmUVD_CGC_GATE, data3);
}

// uvd_v6_0_set_sw_clock_gating
void AmdUvd6::set_sw_clock_gating() {
  uint32_t data, data2;

  data = RREG32(mmUVD_CGC_CTRL);
  data2 = RREG32(mmUVD_SUVD_CGC_CTRL);


  data &= ~(UVD_CGC_CTRL__CLK_OFF_DELAY_MASK |
	  UVD_CGC_CTRL__CLK_GATE_DLY_TIMER_MASK);


  data |= UVD_CGC_CTRL__DYN_CLOCK_MODE_MASK |
	(1 << REG_FIELD_SHIFT(UVD_CGC_CTRL, CLK_GATE_DLY_TIMER)) |
	(4 << REG_FIELD_SHIFT(UVD_CGC_CTRL, CLK_OFF_DELAY));

  data &= ~(UVD_CGC_CTRL__UDEC_RE_MODE_MASK |
		UVD_CGC_CTRL__UDEC_CM_MODE_MASK |
		UVD_CGC_CTRL__UDEC_IT_MODE_MASK |
		UVD_CGC_CTRL__UDEC_DB_MODE_MASK |
		UVD_CGC_CTRL__UDEC_MP_MODE_MASK |
		UVD_CGC_CTRL__SYS_MODE_MASK |
		UVD_CGC_CTRL__UDEC_MODE_MASK |
		UVD_CGC_CTRL__MPEG2_MODE_MASK |
		UVD_CGC_CTRL__REGS_MODE_MASK |
		UVD_CGC_CTRL__RBC_MODE_MASK |
		UVD_CGC_CTRL__LMI_MC_MODE_MASK |
		UVD_CGC_CTRL__LMI_UMC_MODE_MASK |
		UVD_CGC_CTRL__IDCT_MODE_MASK |
		UVD_CGC_CTRL__MPRD_MODE_MASK |
		UVD_CGC_CTRL__MPC_MODE_MASK |
		UVD_CGC_CTRL__LBSI_MODE_MASK |
		UVD_CGC_CTRL__LRBBM_MODE_MASK |
		UVD_CGC_CTRL__WCB_MODE_MASK |
		UVD_CGC_CTRL__VCPU_MODE_MASK |
		UVD_CGC_CTRL__JPEG_MODE_MASK |
		UVD_CGC_CTRL__SCPU_MODE_MASK |
		UVD_CGC_CTRL__JPEG2_MODE_MASK);
  data2 &= ~(UVD_SUVD_CGC_CTRL__SRE_MODE_MASK |
		UVD_SUVD_CGC_CTRL__SIT_MODE_MASK |
		UVD_SUVD_CGC_CTRL__SMP_MODE_MASK |
		UVD_SUVD_CGC_CTRL__SCM_MODE_MASK |
		UVD_SUVD_CGC_CTRL__SDB_MODE_MASK);

  WREG32(mmUVD_CGC_CTRL, data);
  WREG32(mmUVD_SUVD_CGC_CTRL, data2);
}

// uvd_v6_0_mc_resume (the direct-load branch: the driver loads the microcode itself, as amdgpu_uvd_resume copies it)
void AmdUvd6::mc_resume(const AmdVcnFirmware& firmware, const Memory& memory) {
  uint64_t offset;
  uint32_t size;

  /* program memory controller bits 0-27 */
  WREG32(mmUVD_LMI_VCPU_CACHE_64BIT_BAR_LOW,
		lower_32_bits(memory.block_gpu_address));
  WREG32(mmUVD_LMI_VCPU_CACHE_64BIT_BAR_HIGH,
		upper_32_bits(memory.block_gpu_address));

  offset = kFirmwareOffset;
  size = static_cast<uint32_t>(firmware_window_bytes(firmware) - kFirmwareOffset);
  WREG32(mmUVD_VCPU_CACHE_OFFSET0, offset >> 3);
  WREG32(mmUVD_VCPU_CACHE_SIZE0, size);

  offset += size;
  size = kHeapBytes;
  WREG32(mmUVD_VCPU_CACHE_OFFSET1, offset >> 3);
  WREG32(mmUVD_VCPU_CACHE_SIZE1, size);

  offset += size;
  size = kStackBytes +
	 (kSessionBytes * config_.max_handles);
  WREG32(mmUVD_VCPU_CACHE_OFFSET2, offset >> 3);
  WREG32(mmUVD_VCPU_CACHE_SIZE2, size);

  WREG32(mmUVD_UDEC_ADDR_CONFIG, config_.gb_addr_config);
  WREG32(mmUVD_UDEC_DB_ADDR_CONFIG, config_.gb_addr_config);
  WREG32(mmUVD_UDEC_DBW_ADDR_CONFIG, config_.gb_addr_config);

  WREG32(mmUVD_GP_SCRATCH4, config_.max_handles);
}

// uvd_v6_0_start (decode ring only: not ported: the encode rings of uvd_v6_0_enc_support)
AmdUvd6::Status AmdUvd6::run_start(const AmdVcnFirmware& firmware, const Memory& memory) {
  uint32_t rb_bufsz, tmp;
  uint32_t lmi_swap_cntl;
  uint32_t mp_swap_cntl;
  int i, j, r = 0;
  uint64_t ring_gpu_addr = memory.ring_gpu_address;
  uint64_t ring_wptr;

  /* disable DPG */
  WREG32_P(mmUVD_POWER_STATUS, 0, ~UVD_POWER_STATUS__UVD_PG_MODE_MASK);

  /* disable byte swapping */
  lmi_swap_cntl = 0;
  mp_swap_cntl = 0;

  mc_resume(firmware, memory);

  /* disable interupt */
  WREG32_FIELD(UVD_MASTINT_EN, VCPU_EN, 0);

  /* stall UMC and register bus before resetting VCPU */
  WREG32_FIELD(UVD_LMI_CTRL2, STALL_ARB_UMC, 1);
  mdelay(1);

  /* put LMI, VCPU, RBC etc... into reset */
  WREG32(mmUVD_SOFT_RESET,
	UVD_SOFT_RESET__LMI_SOFT_RESET_MASK |
	UVD_SOFT_RESET__VCPU_SOFT_RESET_MASK |
	UVD_SOFT_RESET__LBSI_SOFT_RESET_MASK |
	UVD_SOFT_RESET__RBC_SOFT_RESET_MASK |
	UVD_SOFT_RESET__CSM_SOFT_RESET_MASK |
	UVD_SOFT_RESET__CXW_SOFT_RESET_MASK |
	UVD_SOFT_RESET__TAP_SOFT_RESET_MASK |
	UVD_SOFT_RESET__LMI_UMC_SOFT_RESET_MASK);
  mdelay(5);

  /* take UVD block out of reset */
  WREG32_FIELD(SRBM_SOFT_RESET, SOFT_RESET_UVD, 0);
  mdelay(5);

  /* initialize UVD memory controller */
  WREG32(mmUVD_LMI_CTRL,
	(0x40 << UVD_LMI_CTRL__WRITE_CLEAN_TIMER__SHIFT) |
	UVD_LMI_CTRL__WRITE_CLEAN_TIMER_EN_MASK |
	UVD_LMI_CTRL__DATA_COHERENCY_EN_MASK |
	UVD_LMI_CTRL__VCPU_DATA_COHERENCY_EN_MASK |
	UVD_LMI_CTRL__REQ_MODE_MASK |
	UVD_LMI_CTRL__DISABLE_ON_FWV_FAIL_MASK);

  WREG32(mmUVD_LMI_SWAP_CNTL, lmi_swap_cntl);
  WREG32(mmUVD_MP_SWAP_CNTL, mp_swap_cntl);

  WREG32(mmUVD_MPC_SET_MUXA0, 0x40c2040);
  WREG32(mmUVD_MPC_SET_MUXA1, 0x0);
  WREG32(mmUVD_MPC_SET_MUXB0, 0x40c2040);
  WREG32(mmUVD_MPC_SET_MUXB1, 0x0);
  WREG32(mmUVD_MPC_SET_ALU, 0);
  WREG32(mmUVD_MPC_SET_MUX, 0x88);

  /* take all subblocks out of reset, except VCPU */
  WREG32(mmUVD_SOFT_RESET, UVD_SOFT_RESET__VCPU_SOFT_RESET_MASK);
  mdelay(5);

  /* enable VCPU clock */
  WREG32(mmUVD_VCPU_CNTL, UVD_VCPU_CNTL__CLK_EN_MASK);

  /* enable UMC */
  WREG32_FIELD(UVD_LMI_CTRL2, STALL_ARB_UMC, 0);

  /* boot up the VCPU */
  WREG32(mmUVD_SOFT_RESET, 0);
  mdelay(10);

  for (i = 0; i < static_cast<int>(config_.boot_attempts ? config_.boot_attempts : 1); ++i) {
    uint32_t status;

    for (j = 0; j < 100; ++j) {
      status = RREG32(mmUVD_STATUS);
      if (status & 2)
	break;
      mdelay(10);
    }
    r = 0;
    if (status & 2)
      break;

    /* UVD not responding, trying to reset the VCPU */
    WREG32_FIELD(UVD_SOFT_RESET, VCPU_SOFT_RESET, 1);
    mdelay(10);
    WREG32_FIELD(UVD_SOFT_RESET, VCPU_SOFT_RESET, 0);
    mdelay(10);
    r = -1;
  }

  if (r) {
    /* UVD not responding, giving up */
    return Status::TimedOut;
  }
  /* enable master interrupt */
  WREG32_P(mmUVD_MASTINT_EN,
	(UVD_MASTINT_EN__VCPU_EN_MASK|UVD_MASTINT_EN__SYS_EN_MASK),
	~(UVD_MASTINT_EN__VCPU_EN_MASK|UVD_MASTINT_EN__SYS_EN_MASK));

  /* clear the bit 4 of UVD_STATUS */
  WREG32_P(mmUVD_STATUS, 0, ~(2 << UVD_STATUS__VCPU_REPORT__SHIFT));

  /* force RBC into idle state */
  rb_bufsz = order_base_2(kRingBytes);
  tmp = REG_SET_FIELD(0, UVD_RBC_RB_CNTL, RB_BUFSZ, rb_bufsz);
  tmp = REG_SET_FIELD(tmp, UVD_RBC_RB_CNTL, RB_BLKSZ, 1);
  tmp = REG_SET_FIELD(tmp, UVD_RBC_RB_CNTL, RB_NO_FETCH, 1);
  tmp = REG_SET_FIELD(tmp, UVD_RBC_RB_CNTL, RB_WPTR_POLL_EN, 0);
  tmp = REG_SET_FIELD(tmp, UVD_RBC_RB_CNTL, RB_NO_UPDATE, 1);
  tmp = REG_SET_FIELD(tmp, UVD_RBC_RB_CNTL, RB_RPTR_WR_EN, 1);
  WREG32(mmUVD_RBC_RB_CNTL, tmp);

  /* set the write pointer delay */
  WREG32(mmUVD_RBC_RB_WPTR_CNTL, 0);

  /* set the wb address */
  WREG32(mmUVD_RBC_RB_RPTR_ADDR, (upper_32_bits(ring_gpu_addr) >> 2));

  /* program the RB_BASE for ring buffer */
  WREG32(mmUVD_LMI_RBC_RB_64BIT_BAR_LOW,
		lower_32_bits(ring_gpu_addr));
  WREG32(mmUVD_LMI_RBC_RB_64BIT_BAR_HIGH,
		upper_32_bits(ring_gpu_addr));

  /* Initialize the ring buffer's read and write pointers */
  WREG32(mmUVD_RBC_RB_RPTR, 0);

  ring_wptr = RREG32(mmUVD_RBC_RB_RPTR);
  WREG32(mmUVD_RBC_RB_WPTR, lower_32_bits(ring_wptr));

  WREG32_FIELD(UVD_RBC_RB_CNTL, RB_NO_FETCH, 0);

  return Status::Ok;
}

AmdUvd6::Status AmdUvd6::start(const AmdVcnFirmware& firmware, const Memory& memory, const Config& config) {
  if (!firmware.loaded() || firmware.header().ip_version_major != 6) return Status::BadFirmware;
  if (config.max_handles == 0 || config.max_handles > kMaxHandles) return Status::BadMemory;
  const uint64_t block = block_bytes(firmware, config.max_handles);
  auto fits = [](uint64_t gpu_address, const MmioRegion& window, uint64_t bytes) {
    return gpu_address != 0 && gpu_address % kPageSize == 0 && gpu_address <= UINT64_MAX - bytes && window.length() >= bytes && window.address() % 4 == 0;
  };
  if (!fits(memory.block_gpu_address, memory.block, block) || !fits(memory.ring_gpu_address, memory.ring, kRingBytes)) return Status::BadMemory;
  // The two ranges, as the GPU sees them, must not overlap: the engine writes to both.
  if (memory.block_gpu_address < memory.ring_gpu_address + kRingBytes && memory.ring_gpu_address < memory.block_gpu_address + block) return Status::BadMemory;

  // amdgpu_uvd_resume: the microcode at the start of the block, everything after it cleared.
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
  for (uint64_t o = 0; o < kRingBytes; o += 4) memory.ring.write32(o, 0);
  // Not in the driver, which flushes through its ring: what was just written through the card's memory BAR is buffered by the host data path, and the engine reads it
  // from the memory controller's side. The flush is vi_flush_hdp without a ring.
  WREG32(mmHDP_MEM_COHERENCY_FLUSH_CNTL, 1);
  RREG32(mmHDP_MEM_COHERENCY_FLUSH_CNTL);

  config_ = config;
  // uvd_v6_0_hw_init, then the power gating state "ungate" that starts the block. Not ported: amdgpu_asic_set_uvd_clocks (the clocks are the SMC's to set).
  enable_clock_gating(false);   // uvd_v6_0_set_clockgating_state(AMD_CG_STATE_UNGATE) ...
  set_sw_clock_gating();
  enable_mgcg(true);
  const Status status = run_start(firmware, memory);
  if (status != Status::Ok) {
    stop();   // the driver's hw_fini after a failed hw_init
    return status;
  }
  started_ = true;
  return Status::Ok;
}

// uvd_v6_0_stop
void AmdUvd6::stop() {
  /* force RBC into idle state */
  WREG32(mmUVD_RBC_RB_CNTL, 0x11010101);

  /* Stall UMC and register bus before resetting VCPU */
  WREG32_P(mmUVD_LMI_CTRL2, 1 << 8, ~(1 << 8));
  mdelay(1);

  /* put VCPU into reset */
  WREG32(mmUVD_SOFT_RESET, UVD_SOFT_RESET__VCPU_SOFT_RESET_MASK);
  mdelay(5);

  /* disable VCPU clock */
  WREG32(mmUVD_VCPU_CNTL, 0x0);

  /* Unstall UMC and register bus */
  WREG32_P(mmUVD_LMI_CTRL2, 0, ~(1 << 8));

  WREG32(mmUVD_STATUS, 0);
  started_ = false;
}

}  // namespace hardware
