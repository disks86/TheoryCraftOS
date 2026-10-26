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


// Follows the Linux amdgpu driver (Copyright Advanced Micro Devices, Inc.; MIT license): gfx_v8_0.c (gfx_v8_0_hw_init, _init_golden_registers, _rlc_stop, _rlc_reset, _rlc_start,
// _cp_resume, _cp_gfx_resume, _cp_gfx_start, _cp_gfx_enable, _ring_test_ring, _hw_fini) and amdgpu_ring.c (amdgpu_ring_commit).
#include "Hardware/GraphicsProcessingUnit/AMD/AmdGfx8.hpp"
#include "Hardware/VideoCodec/AMD/AmdGmc8Memory.hpp"

namespace hardware {

namespace {
// The register numbers and field masks this file uses, generated from gfx_8_0_d.h and gfx_8_0_sh_mask.h by scripts/gen_vcn1_regs.py --vi.
#include "Hardware/GraphicsProcessingUnit/AMD/AmdGfx8Registers.inc"

#define REG_SET_FIELD(orig, reg, field, val) (((orig) & ~reg##__##field##_MASK) | (reg##__##field##_MASK & ((val) << reg##__##field##__SHIFT)))

// golden_settings_vegam_a11 and vegam_golden_common_all: register, the bits the setting owns, the value (amdgpu_device_program_register_sequence).
struct GoldenRegister {
  uint32_t reg, and_mask, or_mask;
};
constexpr GoldenRegister kGoldenVegam[] = {
    {mmCB_HW_CONTROL, 0x0001f3cf, 0x00007208},
    {mmCB_HW_CONTROL_2, 0x0f000000, 0x0d000000},
    {mmCB_HW_CONTROL_3, 0x000001ff, 0x00000040},
    {mmDB_DEBUG2, 0xf00fffff, 0x00000400},
    {mmPA_SC_ENHANCE, 0xffffffff, 0x20000001},
    {mmPA_SC_LINE_STIPPLE_STATE, 0x0000ff0f, 0x00000000},
    {mmPA_SC_RASTER_CONFIG, 0x3f3fffff, 0x3a00161a},
    {mmPA_SC_RASTER_CONFIG_1, 0x0000003f, 0x0000002e},
    {mmRLC_CGCG_CGLS_CTRL, 0x00000003, 0x0001003c},
    {mmRLC_CGCG_CGLS_CTRL_3D, 0xffffffff, 0x0001003c},
    {mmSQ_CONFIG, 0x07f80000, 0x01180000},
    {mmTA_CNTL_AUX, 0x000f000f, 0x000b0000},
    {mmTCC_CTRL, 0x00100000, 0xf31fff7f},
    {mmTCP_ADDR_CONFIG, 0x000003ff, 0x000000f7},
    {mmTCP_CHAN_STEER_HI, 0xffffffff, 0x00000000},
    {mmTCP_CHAN_STEER_LO, 0xffffffff, 0x32761054},
    {mmVGT_RESET_DEBUG, 0x00000004, 0x00000004},
    {mmGRBM_GFX_INDEX, 0xffffffff, 0xe0000000},
    {mmGB_ADDR_CONFIG, 0xffffffff, 0x22011003},
    {mmSPI_RESOURCE_RESERVE_CU_0, 0xffffffff, 0x00000800},
    {mmSPI_RESOURCE_RESERVE_CU_1, 0xffffffff, 0x00000800},
    {mmSPI_RESOURCE_RESERVE_EN_CU_0, 0xffffffff, 0x00FF7FBF},
    {mmSPI_RESOURCE_RESERVE_EN_CU_1, 0xffffffff, 0x00FF7FAF},
};

// The command processor's packets (vid.h): type 3, opcode, count (the number of dwords after the header, minus one).
constexpr uint32_t packet3(uint32_t opcode, uint32_t count) { return (3u << 30) | ((opcode & 0xFF) << 8) | ((count & 0x3FFF) << 16); }
constexpr uint32_t kPacketNop = 0x10, kPacketSetBase = 0x11, kPacketClearState = 0x12, kPacketContextControl = 0x28, kPacketPreambleCntl = 0x4A;
constexpr uint32_t kPacketSetContextReg = 0x69, kPacketSetUconfigReg = 0x79;
constexpr uint32_t kSetContextRegStart = 0xA000, kSetUconfigRegStart = 0xC000;
constexpr uint32_t kPreambleBeginClearState = 2u << 28, kPreambleEndClearState = 3u << 28;
constexpr uint32_t kCePartitionBase = 3;   // PACKET3_BASE_INDEX(CE_PARTITION_BASE)
constexpr uint32_t kRingNop = packet3(kPacketNop, 0x3FFF);

constexpr uint32_t kTestInitial = 0xCAFEDEAD, kTestValue = 0xDEADBEEF;
}  // namespace

const char* AmdGfx8::result_name(Result result) {
  switch (result) {
    case Result::Ok: return "ok";
    case Result::BadArgument: return "bad argument";
    case Result::RingTestFailed: return "ring test failed";
  }
  return "?";
}

void AmdGfx8::delay_us(uint32_t us) {
  const uint64_t begin = clock_.now_us();
  while (clock_.now_us() - begin < us) {}
}

// amdgpu_device_program_register_sequence, for the families before AI: a mask of all ones is a plain write, else the owned bits are cleared and the value is ORed in.
void AmdGfx8::program_golden_registers() {
  for (const GoldenRegister& g : kGoldenVegam) {
    uint32_t value = g.or_mask;
    if (g.and_mask != 0xFFFFFFFFu) value = (rd(g.reg) & ~g.and_mask) | g.or_mask;
    wr(g.reg, value);
  }
}

// gfx_v8_0_select_se_sh with every instance and VMID 0.
void AmdGfx8::select_se_sh(uint32_t se, uint32_t sh) {
  uint32_t data = REG_SET_FIELD(0, GRBM_GFX_INDEX, INSTANCE_BROADCAST_WRITES, 1);
  data = se == 0xFFFFFFFFu ? REG_SET_FIELD(data, GRBM_GFX_INDEX, SE_BROADCAST_WRITES, 1) : REG_SET_FIELD(data, GRBM_GFX_INDEX, SE_INDEX, se);
  data = sh == 0xFFFFFFFFu ? REG_SET_FIELD(data, GRBM_GFX_INDEX, SH_BROADCAST_WRITES, 1) : REG_SET_FIELD(data, GRBM_GFX_INDEX, SH_INDEX, sh);
  wr(mmGRBM_GFX_INDEX, data);
}

void AmdGfx8::wait_for_rlc_serdes(uint32_t timeout_us) {
  for (uint32_t i = 0; i < config_.shader_engines; ++i) {
    for (uint32_t j = 0; j < config_.sh_per_se; ++j) {
      select_se_sh(i, j);
      const uint64_t begin = clock_.now_us();
      while (rd(mmRLC_SERDES_CU_MASTER_BUSY) != 0) {
        if (clock_.now_us() - begin >= timeout_us) {
          select_se_sh(0xFFFFFFFFu, 0xFFFFFFFFu);   // the driver gives up here and leaves the selection broadcast
          return;
        }
      }
    }
  }
  select_se_sh(0xFFFFFFFFu, 0xFFFFFFFFu);
  const uint32_t mask = RLC_SERDES_NONCU_MASTER_BUSY__SE_MASTER_BUSY_MASK | RLC_SERDES_NONCU_MASTER_BUSY__GC_MASTER_BUSY_MASK | RLC_SERDES_NONCU_MASTER_BUSY__TC0_MASTER_BUSY_MASK |
                        RLC_SERDES_NONCU_MASTER_BUSY__TC1_MASTER_BUSY_MASK;
  const uint64_t begin = clock_.now_us();
  while ((rd(mmRLC_SERDES_NONCU_MASTER_BUSY) & mask) != 0) {
    if (clock_.now_us() - begin >= timeout_us) return;
  }
}

void AmdGfx8::gui_idle_interrupt(bool enable) {
  uint32_t tmp = rd(mmCP_INT_CNTL_RING0);
  const uint32_t bits = CP_INT_CNTL_RING0__CNTX_BUSY_INT_ENABLE_MASK | CP_INT_CNTL_RING0__CNTX_EMPTY_INT_ENABLE_MASK | CP_INT_CNTL_RING0__CMP_BUSY_INT_ENABLE_MASK |
                        CP_INT_CNTL_RING0__GFX_IDLE_INT_ENABLE_MASK;
  tmp = enable ? (tmp | bits) : (tmp & ~bits);
  wr(mmCP_INT_CNTL_RING0, tmp);
}

void AmdGfx8::rlc_stop(uint32_t timeout_us) {
  wr(mmRLC_CNTL, rd(mmRLC_CNTL) & ~RLC_CNTL__RLC_ENABLE_F32_MASK);
  gui_idle_interrupt(false);
  wait_for_rlc_serdes(timeout_us);
}

void AmdGfx8::rlc_reset() {
  wr(mmGRBM_SOFT_RESET, rd(mmGRBM_SOFT_RESET) | GRBM_SOFT_RESET__SOFT_RESET_RLC_MASK);
  delay_us(50);
  wr(mmGRBM_SOFT_RESET, rd(mmGRBM_SOFT_RESET) & ~GRBM_SOFT_RESET__SOFT_RESET_RLC_MASK);
  delay_us(50);
}

void AmdGfx8::rlc_start() {
  wr(mmRLC_CNTL, rd(mmRLC_CNTL) | RLC_CNTL__RLC_ENABLE_F32_MASK);
  gui_idle_interrupt(true);   // a card that is not an APU enables the CP interrupt here
  delay_us(50);
}

void AmdGfx8::cp_gfx_enable(bool enable) {
  const uint32_t halts = CP_ME_CNTL__ME_HALT_MASK | CP_ME_CNTL__PFP_HALT_MASK | CP_ME_CNTL__CE_HALT_MASK;
  const uint32_t tmp = rd(mmCP_ME_CNTL);
  wr(mmCP_ME_CNTL, enable ? tmp & ~halts : tmp | halts);
  delay_us(50);
}

uint32_t AmdGfx8::build_start_stream(const Config& config, ArrayView<uint32_t> out) {
  const uint32_t stream[] = {
      packet3(kPacketPreambleCntl, 0), kPreambleBeginClearState,
      packet3(kPacketContextControl, 1), 0x80000000u, 0x80000000u,
      packet3(kPacketSetContextReg, 2), mmPA_SC_RASTER_CONFIG - kSetContextRegStart, config.raster_config, config.raster_config_1,
      packet3(kPacketPreambleCntl, 0), kPreambleEndClearState,
      packet3(kPacketClearState, 0), 0,
      packet3(kPacketSetBase, 2), kCePartitionBase, 0x8000, 0x8000,
  };
  const uint32_t count = sizeof(stream) / sizeof(stream[0]);
  if (out.size() < count) return 0;
  for (uint32_t i = 0; i < count; ++i) out[i] = stream[i];
  return count;
}

void AmdGfx8::ring_write(uint32_t dword) {
  memory_.ring.write32(uint64_t{wptr_} * 4, dword);
  wptr_ = (wptr_ + 1) & (kRingDwords - 1);
}

// amdgpu_ring_commit: pad to the alignment with NOPs, flush the host data path, publish the write pointer (gfx_v8_0_ring_set_wptr, register form).
void AmdGfx8::ring_commit() {
  const uint32_t count = (kAlignMask + 1 - (wptr_ & kAlignMask)) & kAlignMask;
  for (uint32_t i = 0; i < count; ++i) ring_write(kRingNop);
  amd_gmc8_flush_hdp(registers_);
  wr(mmCP_RB0_WPTR, wptr_);
}

// gfx_v8_0_cp_gfx_resume
void AmdGfx8::cp_resume_ring(const Memory& memory) {
  wr(mmCP_RB_WPTR_DELAY, 0);
  wr(mmCP_RB_VMID, 0);
  // The ring is 2^n dwords: the size field is log2 of the size in 8-byte units.
  uint32_t rb_bufsz = 0;
  while ((1u << rb_bufsz) < kRingBytes / 8) ++rb_bufsz;
  uint32_t tmp = REG_SET_FIELD(0, CP_RB0_CNTL, RB_BUFSZ, rb_bufsz);
  tmp = REG_SET_FIELD(tmp, CP_RB0_CNTL, RB_BLKSZ, rb_bufsz - 2);
  tmp = REG_SET_FIELD(tmp, CP_RB0_CNTL, MTYPE, 3);
  tmp = REG_SET_FIELD(tmp, CP_RB0_CNTL, MIN_IB_AVAILSZ, 1);
  wr(mmCP_RB0_CNTL, tmp);
  // The read and write pointers are written to zero while the read pointer is writable.
  wr(mmCP_RB0_CNTL, tmp | CP_RB0_CNTL__RB_RPTR_WR_ENA_MASK);
  wptr_ = 0;
  wr(mmCP_RB0_WPTR, 0);
  // Where the CP writes its read pointer, and the write pointer poll address (both in the write-back memory).
  const uint64_t rptr_addr = memory.writeback_gpu_address, wptr_addr = memory.writeback_gpu_address + 8;
  wr(mmCP_RB0_RPTR_ADDR, static_cast<uint32_t>(rptr_addr));
  wr(mmCP_RB0_RPTR_ADDR_HI, static_cast<uint32_t>(rptr_addr >> 32) & 0xFF);
  wr(mmCP_RB_WPTR_POLL_ADDR_LO, static_cast<uint32_t>(wptr_addr));
  wr(mmCP_RB_WPTR_POLL_ADDR_HI, static_cast<uint32_t>(wptr_addr >> 32));
  delay_us(1000);
  wr(mmCP_RB0_CNTL, tmp);
  const uint64_t base = memory.ring_gpu_address >> 8;
  wr(mmCP_RB0_BASE, static_cast<uint32_t>(base));
  wr(mmCP_RB0_BASE_HI, static_cast<uint32_t>(base >> 32));
  // gfx_v8_0_set_cpg_door_bell: no doorbell (the write pointer goes through CP_RB0_WPTR).
  wr(mmCP_RB_DOORBELL_CONTROL, rd(mmCP_RB_DOORBELL_CONTROL) & ~CP_RB_DOORBELL_CONTROL__DOORBELL_EN_MASK);
  // amdgpu_ring_clear_ring
  for (uint32_t i = 0; i < kRingDwords; ++i) memory_.ring.write32(uint64_t{i} * 4, 0);
  // gfx_v8_0_cp_gfx_start
  wr(mmCP_MAX_CONTEXT, config_.max_hw_contexts - 1);
  wr(mmCP_ENDIAN_SWAP, 0);
  wr(mmCP_DEVICE_ID, 1);
  cp_gfx_enable(true);
  uint32_t stream[32];
  const uint32_t count = build_start_stream(config_, ArrayView<uint32_t>(stream, 32));
  for (uint32_t i = 0; i < count; ++i) ring_write(stream[i]);
  ring_commit();
}

// gfx_v8_0_ring_test_ring
bool AmdGfx8::test_ring(uint32_t timeout_us) {
  wr(mmSCRATCH_REG0, kTestInitial);
  ring_write(packet3(kPacketSetUconfigReg, 1));
  ring_write(mmSCRATCH_REG0 - kSetUconfigRegStart);
  ring_write(kTestValue);
  ring_commit();
  const uint64_t begin = clock_.now_us();
  do {
    if (rd(mmSCRATCH_REG0) == kTestValue) return true;
  } while (clock_.now_us() - begin < timeout_us);
  return false;
}

AmdGfx8::Result AmdGfx8::start(const Memory& memory, uint32_t timeout_us) {
  if (!memory.ring.valid() || memory.ring.length() < kRingBytes || !memory.writeback.valid() || memory.writeback.length() < kWritebackBytes ||
      (memory.ring_gpu_address & 0xFF) != 0 || (memory.writeback_gpu_address & 3) != 0 || config_.shader_engines == 0 || config_.sh_per_se == 0 ||
      config_.max_hw_contexts == 0)
    return Result::BadArgument;
  memory_ = memory;
  // gfx_v8_0_hw_init
  program_golden_registers();
  rlc_stop(timeout_us);
  rlc_reset();
  rlc_start();
  // gfx_v8_0_cp_resume
  gui_idle_interrupt(false);
  cp_resume_ring(memory);
  if (!test_ring(timeout_us)) return Result::RingTestFailed;
  gui_idle_interrupt(true);
  return Result::Ok;
}

void AmdGfx8::stop() {
  cp_gfx_enable(false);
  wr(mmCP_MEC_CNTL, CP_MEC_CNTL__MEC_ME1_HALT_MASK | CP_MEC_CNTL__MEC_ME2_HALT_MASK);   // gfx_v8_0_cp_compute_enable(false)
  delay_us(50);
  rlc_stop(kDefaultTimeoutUs);
}

}  // namespace hardware
