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

// Follows the Linux amdgpu driver (Copyright Advanced Micro Devices, Inc.; MIT license), kernel 6.12: gmc_v9_0.c (gmc_v9_0_hw_init), mmhub_v1_0.c (get_fb_location,
// init_system_aperture_regs, init_tlb_regs), hdp_v4_0.c, nbio_v6_1.c / nbio_v7_4.c, and the display core: dce120_hwseq.c (dce120_update_dchub), dce120_resource.c
// (read_pipe_fuses), dce120_timing_generator.c, dce_mem_input.c, dce_transform.c, dce110_hwseq.c (dce110_set_safe_displaymarks), dce_link_encoder.c, dce_stream_encoder.c,
// bios/command_table2.c and command_table_helper2_dce112.c.
#include "Hardware/GraphicsProcessingUnit/AMD/AmdDce12.hpp"

namespace hardware {
namespace amd_dce12 {

namespace {
// The register numbers and field masks this file uses, generated from dce_12_0_offset.h, dce_12_0_sh_mask.h, mmhub_1_0_offset.h, mmhub_1_0_sh_mask.h, nbio_6_1_offset.h,
// nbio_6_1_sh_mask.h, hdp_4_0_offset.h and hdp_4_0_sh_mask.h by scripts/gen_vcn1_regs.py.
#include "Hardware/GraphicsProcessingUnit/AMD/AmdDce12Registers.inc"

// A field of a register value: the mask's lowest bit is the shift (every field used here is a contiguous run of bits).
constexpr uint32_t shift_of(uint32_t mask) {
  uint32_t s = 0;
  while (s < 31 && !(mask >> s & 1)) ++s;
  return s;
}
constexpr uint32_t set_field(uint32_t value, uint32_t mask, uint32_t field) { return (value & ~mask) | ((field << shift_of(mask)) & mask); }
constexpr uint32_t get_field(uint32_t value, uint32_t mask) { return (value & mask) >> shift_of(mask); }

constexpr uint32_t dce(uint32_t base_idx, uint32_t reg) { return kDceBase[base_idx] + reg; }

// The register blocks of the six pipes, the seven DIGs and the six hot plug pins, from the instance registers (dce120_resource.c's SRI lists; each block keeps its registers
// at the same distances, the stride of one instance to the next).
constexpr uint32_t kDcp[kMaxPipes] = {0, mmDCP1_GRPH_ENABLE - mmDCP0_GRPH_ENABLE, mmDCP2_GRPH_ENABLE - mmDCP0_GRPH_ENABLE, mmDCP3_GRPH_ENABLE - mmDCP0_GRPH_ENABLE,
                                      mmDCP4_GRPH_ENABLE - mmDCP0_GRPH_ENABLE, mmDCP5_GRPH_ENABLE - mmDCP0_GRPH_ENABLE};
constexpr uint32_t kCrtc[kMaxPipes] = {0, mmCRTC1_CRTC_BLANK_CONTROL - mmCRTC0_CRTC_BLANK_CONTROL, mmCRTC2_CRTC_BLANK_CONTROL - mmCRTC0_CRTC_BLANK_CONTROL,
                                       mmCRTC3_CRTC_BLANK_CONTROL - mmCRTC0_CRTC_BLANK_CONTROL, mmCRTC4_CRTC_BLANK_CONTROL - mmCRTC0_CRTC_BLANK_CONTROL,
                                       mmCRTC5_CRTC_BLANK_CONTROL - mmCRTC0_CRTC_BLANK_CONTROL};
constexpr uint32_t kScl[kMaxPipes] = {0, mmSCL1_SCL_MODE - mmSCL0_SCL_MODE, mmSCL2_SCL_MODE - mmSCL0_SCL_MODE, mmSCL3_SCL_MODE - mmSCL0_SCL_MODE, mmSCL4_SCL_MODE - mmSCL0_SCL_MODE,
                                      mmSCL5_SCL_MODE - mmSCL0_SCL_MODE};
constexpr uint32_t kLb[kMaxPipes] = {0, mmLB1_LB_DATA_FORMAT - mmLB0_LB_DATA_FORMAT, mmLB2_LB_DATA_FORMAT - mmLB0_LB_DATA_FORMAT, mmLB3_LB_DATA_FORMAT - mmLB0_LB_DATA_FORMAT,
                                     mmLB4_LB_DATA_FORMAT - mmLB0_LB_DATA_FORMAT, mmLB5_LB_DATA_FORMAT - mmLB0_LB_DATA_FORMAT};
constexpr uint32_t kFmt[kMaxPipes] = {0, mmFMT1_FMT_BIT_DEPTH_CONTROL - mmFMT0_FMT_BIT_DEPTH_CONTROL, mmFMT2_FMT_BIT_DEPTH_CONTROL - mmFMT0_FMT_BIT_DEPTH_CONTROL,
                                      mmFMT3_FMT_BIT_DEPTH_CONTROL - mmFMT0_FMT_BIT_DEPTH_CONTROL, mmFMT4_FMT_BIT_DEPTH_CONTROL - mmFMT0_FMT_BIT_DEPTH_CONTROL,
                                      mmFMT5_FMT_BIT_DEPTH_CONTROL - mmFMT0_FMT_BIT_DEPTH_CONTROL};
constexpr uint32_t kDmif[kMaxPipes] = {0, mmDMIF_PG1_DPG_WATERMARK_MASK_CONTROL - mmDMIF_PG0_DPG_WATERMARK_MASK_CONTROL,
                                       mmDMIF_PG2_DPG_WATERMARK_MASK_CONTROL - mmDMIF_PG0_DPG_WATERMARK_MASK_CONTROL,
                                       mmDMIF_PG3_DPG_WATERMARK_MASK_CONTROL - mmDMIF_PG0_DPG_WATERMARK_MASK_CONTROL,
                                       mmDMIF_PG4_DPG_WATERMARK_MASK_CONTROL - mmDMIF_PG0_DPG_WATERMARK_MASK_CONTROL,
                                       mmDMIF_PG5_DPG_WATERMARK_MASK_CONTROL - mmDMIF_PG0_DPG_WATERMARK_MASK_CONTROL};
constexpr uint32_t kDmifBuffer[kMaxPipes] = {mmPIPE0_DMIF_BUFFER_CONTROL, mmPIPE1_DMIF_BUFFER_CONTROL, mmPIPE2_DMIF_BUFFER_CONTROL,
                                             mmPIPE3_DMIF_BUFFER_CONTROL, mmPIPE4_DMIF_BUFFER_CONTROL, mmPIPE5_DMIF_BUFFER_CONTROL};
constexpr uint32_t kDig[kMaxDigs] = {0, mmDIG1_DIG_FE_CNTL - mmDIG0_DIG_FE_CNTL, mmDIG2_DIG_FE_CNTL - mmDIG0_DIG_FE_CNTL, mmDIG3_DIG_FE_CNTL - mmDIG0_DIG_FE_CNTL,
                                     mmDIG4_DIG_FE_CNTL - mmDIG0_DIG_FE_CNTL, mmDIG5_DIG_FE_CNTL - mmDIG0_DIG_FE_CNTL, mmDIG6_DIG_FE_CNTL - mmDIG0_DIG_FE_CNTL};
constexpr uint32_t kHpd[kMaxHpd] = {0, mmHPD1_DC_HPD_CONTROL - mmHPD0_DC_HPD_CONTROL, mmHPD2_DC_HPD_CONTROL - mmHPD0_DC_HPD_CONTROL, mmHPD3_DC_HPD_CONTROL - mmHPD0_DC_HPD_CONTROL,
                                    mmHPD4_DC_HPD_CONTROL - mmHPD0_DC_HPD_CONTROL, mmHPD5_DC_HPD_CONTROL - mmHPD0_DC_HPD_CONTROL};
constexpr uint32_t kVga[kMaxPipes] = {mmD1VGA_CONTROL, mmD2VGA_CONTROL, mmD3VGA_CONTROL, mmD4VGA_CONTROL, mmD5VGA_CONTROL, mmD6VGA_CONTROL};
static_assert(mmD1VGA_CONTROL_BASE_IDX == 1 && mmD6VGA_CONTROL_BASE_IDX == 1 && mmPIPE5_DMIF_BUFFER_CONTROL_BASE_IDX == 2, "the VGA controls and DMIF buffers' segments");

// The DDC lines' GPIO registers: MASK, A, EN, Y of DC_GPIO_DDC1..6 (all in segment 2)
struct DdcRegs {
  uint32_t mask, a, en, y;
};
constexpr DdcRegs kDdc[kMaxDdc] = {
    {mmDC_GPIO_DDC1_MASK, mmDC_GPIO_DDC1_A, mmDC_GPIO_DDC1_EN, mmDC_GPIO_DDC1_Y}, {mmDC_GPIO_DDC2_MASK, mmDC_GPIO_DDC2_A, mmDC_GPIO_DDC2_EN, mmDC_GPIO_DDC2_Y},
    {mmDC_GPIO_DDC3_MASK, mmDC_GPIO_DDC3_A, mmDC_GPIO_DDC3_EN, mmDC_GPIO_DDC3_Y}, {mmDC_GPIO_DDC4_MASK, mmDC_GPIO_DDC4_A, mmDC_GPIO_DDC4_EN, mmDC_GPIO_DDC4_Y},
    {mmDC_GPIO_DDC5_MASK, mmDC_GPIO_DDC5_A, mmDC_GPIO_DDC5_EN, mmDC_GPIO_DDC5_Y}, {mmDC_GPIO_DDC6_MASK, mmDC_GPIO_DDC6_A, mmDC_GPIO_DDC6_EN, mmDC_GPIO_DDC6_Y},
};
static_assert(mmDC_GPIO_DDC1_MASK_BASE_IDX == 2 && mmDC_GPIO_DDC6_Y_BASE_IDX == 2, "the DDC pads' segment");

constexpr uint64_t kMiB = 1ull << 20;
constexpr uint64_t kMaxVideoMemory = 64ull << 30;
constexpr uint64_t kAgpStartDisabled = 0xFFFFFFFFFFFFull;        // amdgpu_gmc_set_agp_default: agp_start, with agp_end 0
constexpr uint32_t kMtypeUc = 3;                                // MTYPE_UC (soc15_common: enum { MTYPE_NC, MTYPE_WC, MTYPE_CC, MTYPE_UC })
constexpr uint32_t kSystemAccessMode = 3;
constexpr uint32_t kLbMemorySize = 0x6B0;                       // LB_TOTAL_NUMBER_OF_ENTRIES (dce_transform.h)
constexpr uint32_t kDenormMode30Bpp = 3;                        // set_denormalization for COLOR_DEPTH_101010 (the LB_PIXEL_DEPTH_30BPP DC stores in)
constexpr uint32_t kMaxWatermark = 0xFFFF, kSafeNbpMark = 0x7FFF;   // MAX_WATERMARK, SAFE_NBP_MARK (dce110_hwseq.c)
constexpr uint32_t kViewportMax = 0x3FFF;
}  // namespace

// ------------------------------------------------------------------------------------------------------------------------------------- memory

MemoryMap read_memory_map(IVcnRegisters& r) {
  MemoryMap map;
  const uint32_t size = r.read((kNbioBase[mmRCC_PF_0_0_RCC_CONFIG_MEMSIZE_BASE_IDX] + mmRCC_PF_0_0_RCC_CONFIG_MEMSIZE));     // nbio_v6_1_get_memsize: MiB
  if (size == 0 || size == 0xFFFFFFFFu || uint64_t{size} * kMiB > kMaxVideoMemory) return map;
  const uint32_t base = r.read((kMmhubBase[mmMC_VM_FB_LOCATION_BASE_BASE_IDX] + mmMC_VM_FB_LOCATION_BASE)), top = r.read((kMmhubBase[mmMC_VM_FB_LOCATION_TOP_BASE_IDX] + mmMC_VM_FB_LOCATION_TOP));
  const uint32_t offset = r.read((kMmhubBase[mmMC_VM_FB_OFFSET_BASE_IDX] + mmMC_VM_FB_OFFSET));
  if (base == 0xFFFFFFFFu || top == 0xFFFFFFFFu) return map;
  const uint64_t fb_base = uint64_t{base & MC_VM_FB_LOCATION_BASE__FB_BASE_MASK} << 24;
  const uint64_t fb_top = uint64_t{top & MC_VM_FB_LOCATION_TOP__FB_TOP_MASK} << 24;
  const uint64_t bytes = uint64_t{size} * kMiB;
  // The controller's span (TOP is the start of its last 16 MiB) holds the memory, rounded up to 16 MiB, and not much more.
  if (fb_top < fb_base || fb_top - fb_base + 16 * kMiB < bytes || fb_top - fb_base + 16 * kMiB - bytes >= 16 * kMiB) return map;
  map.read = true;
  map.video_memory_bytes = bytes;
  map.gpu_base = fb_base;
  map.gpu_top = fb_top;
  map.fb_offset = uint64_t{offset & MC_VM_FB_OFFSET__FB_OFFSET_MASK} << 24;
  return map;
}

void flush_hdp(IVcnRegisters& r) {
  r.write((kNbioBase[mmBIF_BX_PF0_HDP_MEM_COHERENCY_FLUSH_CNTL_BASE_IDX] + mmBIF_BX_PF0_HDP_MEM_COHERENCY_FLUSH_CNTL), 0);
  (void)r.read((kNbioBase[mmBIF_BX_PF0_HDP_MEM_COHERENCY_FLUSH_CNTL_BASE_IDX] + mmBIF_BX_PF0_HDP_MEM_COHERENCY_FLUSH_CNTL));
}

bool program_memory(IVcnRegisters& r, const MemoryMap& map, uint64_t scratch) {
  if (!map.read || map.video_memory_bytes == 0) return false;
  const uint64_t fb_start = map.gpu_base, fb_end = map.gpu_base + map.video_memory_bytes - 1;     // amdgpu_gmc_vram_location
  if (scratch < fb_start || scratch > fb_end || (scratch & 0xFFF) != 0) return false;
  // gmc_v9_0_hw_init: the VGA aperture locked out, VGA rendering off
  uint32_t tmp = r.read(dce(mmVGA_HDP_CONTROL_BASE_IDX, mmVGA_HDP_CONTROL));
  r.write(dce(mmVGA_HDP_CONTROL_BASE_IDX, mmVGA_HDP_CONTROL), set_field(tmp, VGA_HDP_CONTROL__VGA_MEMORY_DISABLE_MASK, 1));
  tmp = r.read(dce(mmVGA_RENDER_CONTROL_BASE_IDX, mmVGA_RENDER_CONTROL));
  r.write(dce(mmVGA_RENDER_CONTROL_BASE_IDX, mmVGA_RENDER_CONTROL), set_field(tmp, VGA_RENDER_CONTROL__VGA_VSTATUS_CNTL_MASK, 0));
  // hdp_v4_0_init_registers, then the flush
  tmp = r.read((kHdpBase[mmHDP_MISC_CNTL_BASE_IDX] + mmHDP_MISC_CNTL));
  r.write((kHdpBase[mmHDP_MISC_CNTL_BASE_IDX] + mmHDP_MISC_CNTL), set_field(tmp, HDP_MISC_CNTL__FLUSH_INVALIDATE_CACHE_MASK, 1));
  r.write((kHdpBase[mmHDP_NONSURFACE_BASE_BASE_IDX] + mmHDP_NONSURFACE_BASE), static_cast<uint32_t>(fb_start >> 8));
  r.write((kHdpBase[mmHDP_NONSURFACE_BASE_HI_BASE_IDX] + mmHDP_NONSURFACE_BASE_HI), static_cast<uint32_t>(fb_start >> 40));
  flush_hdp(r);
  // mmhub_v1_0_init_system_aperture_regs: the AGP aperture empty, the system aperture over video memory, unmapped accesses to the scratch page
  r.write((kMmhubBase[mmMC_VM_AGP_BASE_BASE_IDX] + mmMC_VM_AGP_BASE), 0);
  r.write((kMmhubBase[mmMC_VM_AGP_BOT_BASE_IDX] + mmMC_VM_AGP_BOT), static_cast<uint32_t>(kAgpStartDisabled >> 24));
  r.write((kMmhubBase[mmMC_VM_AGP_TOP_BASE_IDX] + mmMC_VM_AGP_TOP), 0);
  r.write((kMmhubBase[mmMC_VM_SYSTEM_APERTURE_LOW_ADDR_BASE_IDX] + mmMC_VM_SYSTEM_APERTURE_LOW_ADDR), static_cast<uint32_t>((fb_start < kAgpStartDisabled ? fb_start : kAgpStartDisabled) >> 18));
  r.write((kMmhubBase[mmMC_VM_SYSTEM_APERTURE_HIGH_ADDR_BASE_IDX] + mmMC_VM_SYSTEM_APERTURE_HIGH_ADDR), static_cast<uint32_t>(fb_end >> 18));
  const uint64_t scratch_pa = scratch - fb_start + map.fb_offset;      // amdgpu_gmc_vram_mc2pa
  r.write((kMmhubBase[mmMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_LSB_BASE_IDX] + mmMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_LSB), static_cast<uint32_t>(scratch_pa >> 12));
  r.write((kMmhubBase[mmMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_MSB_BASE_IDX] + mmMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_MSB), static_cast<uint32_t>(scratch_pa >> 44));
  // mmhub_v1_0_init_tlb_regs
  tmp = r.read((kMmhubBase[mmMC_VM_MX_L1_TLB_CNTL_BASE_IDX] + mmMC_VM_MX_L1_TLB_CNTL));
  tmp = set_field(tmp, MC_VM_MX_L1_TLB_CNTL__ENABLE_L1_TLB_MASK, 1);
  tmp = set_field(tmp, MC_VM_MX_L1_TLB_CNTL__SYSTEM_ACCESS_MODE_MASK, kSystemAccessMode);
  tmp = set_field(tmp, MC_VM_MX_L1_TLB_CNTL__ENABLE_ADVANCED_DRIVER_MODEL_MASK, 1);
  tmp = set_field(tmp, MC_VM_MX_L1_TLB_CNTL__SYSTEM_APERTURE_UNMAPPED_ACCESS_MASK, 0);
  tmp = set_field(tmp, MC_VM_MX_L1_TLB_CNTL__MTYPE_MASK, kMtypeUc);
  tmp = set_field(tmp, MC_VM_MX_L1_TLB_CNTL__ATC_EN_MASK, 1);
  r.write((kMmhubBase[mmMC_VM_MX_L1_TLB_CNTL_BASE_IDX] + mmMC_VM_MX_L1_TLB_CNTL), tmp);
  // nbio_v6_1_mc_access_enable (nbio_v7_4: the same register)
  r.write((kNbioBase[mmBIF_FB_EN_BASE_IDX] + mmBIF_FB_EN), BIF_FB_EN__FB_READ_EN_MASK | BIF_FB_EN__FB_WRITE_EN_MASK);
  // dce120_update_dchub, FRAME_BUFFER_MODE_LOCAL_ONLY: the display hub's AGP aperture off (its FB location is the POST's)
  tmp = r.read(dce(mmDCHUB_AGP_BASE_BASE_IDX, mmDCHUB_AGP_BASE));
  r.write(dce(mmDCHUB_AGP_BASE_BASE_IDX, mmDCHUB_AGP_BASE), set_field(tmp, DCHUB_AGP_BASE__AGP_BASE_MASK, 0));
  tmp = r.read(dce(mmDCHUB_AGP_BOT_BASE_IDX, mmDCHUB_AGP_BOT));
  r.write(dce(mmDCHUB_AGP_BOT_BASE_IDX, mmDCHUB_AGP_BOT), set_field(tmp, DCHUB_AGP_BOT__AGP_BOT_MASK, 0x03FFFF));
  tmp = r.read(dce(mmDCHUB_AGP_TOP_BASE_IDX, mmDCHUB_AGP_TOP));
  r.write(dce(mmDCHUB_AGP_TOP_BASE_IDX, mmDCHUB_AGP_TOP), set_field(tmp, DCHUB_AGP_TOP__AGP_TOP_MASK, 0));
  return true;
}

uint32_t pipe_fuses(IVcnRegisters& r) { return r.read(dce(mmCC_DC_PIPE_DIS_BASE_IDX, mmCC_DC_PIPE_DIS)) & 0x3F; }

uint32_t hpd_gpio_register() { return dce(mmDC_GPIO_HPD_A_BASE_IDX, mmDC_GPIO_HPD_A); }

uint32_t ddc_line_of(uint32_t a_register) {
  for (uint32_t i = 0; i < kMaxDdc; ++i)
    if (dce(2, kDdc[i].a) == a_register) return i;
  return kMaxDdc;
}

AtomI2c::Bus ddc_bus(uint32_t line) {
  AtomI2c::Bus b;
  if (line >= kMaxDdc) return b;
  const DdcRegs& d = kDdc[line];
  b.valid = true;
  b.hw_capable = true;     // (a DDC pad pair: its AUX pad mode bit, bit 16 of MASK, is cleared before a transfer)
  b.id = static_cast<uint8_t>(line);
  b.mask_clk_reg = b.mask_data_reg = dce(2, d.mask);
  b.a_clk_reg = b.a_data_reg = dce(2, d.a);
  b.en_clk_reg = b.en_data_reg = dce(2, d.en);
  b.y_clk_reg = b.y_data_reg = dce(2, d.y);
  // the same bit layout on every line (DC_GPIO_DDCn_*: the clock in bit 0, the data in bit 8)
  b.mask_clk_mask = DC_GPIO_DDC1_MASK__DC_GPIO_DDC1CLK_MASK_MASK;
  b.mask_data_mask = DC_GPIO_DDC1_MASK__DC_GPIO_DDC1DATA_MASK_MASK;
  b.a_clk_mask = DC_GPIO_DDC1_A__DC_GPIO_DDC1CLK_A_MASK;
  b.a_data_mask = DC_GPIO_DDC1_A__DC_GPIO_DDC1DATA_A_MASK;
  b.en_clk_mask = DC_GPIO_DDC1_EN__DC_GPIO_DDC1CLK_EN_MASK;
  b.en_data_mask = DC_GPIO_DDC1_EN__DC_GPIO_DDC1DATA_EN_MASK;
  b.y_clk_mask = DC_GPIO_DDC1_Y__DC_GPIO_DDC1CLK_Y_MASK;
  b.y_data_mask = DC_GPIO_DDC1_Y__DC_GPIO_DDC1DATA_Y_MASK;
  return b;
}

// ---------------------------------------------------------------------------------------------------------------------------------------- pipe

uint32_t Pipe::grph_enable(uint32_t crtc) { return crtc < kMaxPipes ? dce(mmDCP0_GRPH_ENABLE_BASE_IDX, mmDCP0_GRPH_ENABLE) + kDcp[crtc] : 0; }
uint32_t Pipe::crtc_blank_control(uint32_t crtc) { return crtc < kMaxPipes ? dce(mmCRTC0_CRTC_BLANK_CONTROL_BASE_IDX, mmCRTC0_CRTC_BLANK_CONTROL) + kCrtc[crtc] : 0; }

bool Pipe::set_blank(uint32_t crtc, bool blank) {
  if (crtc >= kMaxPipes) return false;
  const uint32_t off = kCrtc[crtc];
  r_.write(dce(mmCRTC0_CRTC_DOUBLE_BUFFER_CONTROL_BASE_IDX, mmCRTC0_CRTC_DOUBLE_BUFFER_CONTROL) + off, set_field(0, CRTC0_CRTC_DOUBLE_BUFFER_CONTROL__CRTC_BLANK_DATA_DOUBLE_BUFFER_EN_MASK, 1));
  r_.write(dce(mmCRTC0_CRTC_BLANK_CONTROL_BASE_IDX, mmCRTC0_CRTC_BLANK_CONTROL) + off, blank ? set_field(0, CRTC0_CRTC_BLANK_CONTROL__CRTC_BLANK_DATA_EN_MASK, 1) : 0);
  return true;
}

bool Pipe::blanked(uint32_t crtc) {
  if (crtc >= kMaxPipes) return false;
  const uint32_t v = r_.read(dce(mmCRTC0_CRTC_BLANK_CONTROL_BASE_IDX, mmCRTC0_CRTC_BLANK_CONTROL) + kCrtc[crtc]);
  return get_field(v, CRTC0_CRTC_BLANK_CONTROL__CRTC_BLANK_DATA_EN_MASK) == 1 && get_field(v, CRTC0_CRTC_BLANK_CONTROL__CRTC_CURRENT_BLANK_STATE_MASK) == 1;
}

bool Pipe::prepare_enable(uint32_t crtc) {
  if (crtc >= kMaxPipes) return false;
  const uint32_t mode = dce(mmCRTC0_CRTC_MASTER_UPDATE_MODE_BASE_IDX, mmCRTC0_CRTC_MASTER_UPDATE_MODE) + kCrtc[crtc], lock = dce(mmCRTC0_CRTC_MASTER_UPDATE_LOCK_BASE_IDX, mmCRTC0_CRTC_MASTER_UPDATE_LOCK) + kCrtc[crtc];
  r_.write(mode, set_field(r_.read(mode), CRTC0_CRTC_MASTER_UPDATE_MODE__MASTER_UPDATE_MODE_MASK, 0));
  r_.write(lock, set_field(r_.read(lock), CRTC0_CRTC_MASTER_UPDATE_LOCK__UNDERFLOW_UPDATE_LOCK_MASK, 0));
  return true;
}

bool Pipe::advanced_request(uint32_t crtc, const CrtcMode& m) {
  if (crtc >= kMaxPipes || m.v_total < m.v_sync_start || m.v_sync_start < m.v_display) return false;
  // dce110_timing_generator_apply_front_porch_workaround (progressive): a front porch of at least one line
  uint32_t front_porch = static_cast<uint32_t>(m.v_sync_start - m.v_display);
  if (front_porch < 1) front_porch = 1;
  const uint32_t blank = static_cast<uint32_t>(m.v_total - m.v_display);
  uint32_t lines = blank > front_porch ? blank - front_porch : 0;     // the vertical sync and back porch
  if (lines > 10) lines = 10;
  const uint32_t reg = dce(mmCRTC0_CRTC_START_LINE_CONTROL_BASE_IDX, mmCRTC0_CRTC_START_LINE_CONTROL) + kCrtc[crtc];
  uint32_t v = r_.read(reg);
  v = set_field(v, CRTC0_CRTC_START_LINE_CONTROL__CRTC_LEGACY_REQUESTOR_EN_MASK, 0);
  v = set_field(v, CRTC0_CRTC_START_LINE_CONTROL__CRTC_ADVANCED_START_LINE_POSITION_MASK, lines);
  r_.write(reg, v);
  return true;
}

bool Pipe::disable_vga(uint32_t crtc) {
  if (crtc >= kMaxPipes) return false;
  const uint32_t reg = dce(1, kVga[crtc]);
  uint32_t v = r_.read(reg);
  v = set_field(v, D1VGA_CONTROL__D1VGA_MODE_ENABLE_MASK, 0);
  v = set_field(v, D1VGA_CONTROL__D1VGA_TIMING_SELECT_MASK, 0);
  v = set_field(v, D1VGA_CONTROL__D1VGA_SYNC_POLARITY_SELECT_MASK, 0);
  v = set_field(v, D1VGA_CONTROL__D1VGA_OVERSCAN_COLOR_EN_MASK, 0);
  r_.write(reg, v);
  return true;
}

bool Pipe::program_fmt(uint32_t crtc, uint32_t bpc) {
  if (crtc >= kMaxPipes) return false;
  uint32_t v = 0;
  if (bpc == 6 || bpc == 8 || bpc == 10) {
    v = set_field(v, FMT0_FMT_BIT_DEPTH_CONTROL__FMT_TRUNCATE_EN_MASK, 1);
    v = set_field(v, FMT0_FMT_BIT_DEPTH_CONTROL__FMT_TRUNCATE_DEPTH_MASK, bpc == 6 ? 0 : bpc == 8 ? 1 : 2);
  }
  r_.write(dce(mmFMT0_FMT_BIT_DEPTH_CONTROL_BASE_IDX, mmFMT0_FMT_BIT_DEPTH_CONTROL) + kFmt[crtc], v);
  return true;
}

bool Pipe::set_surface(uint32_t crtc, const amd_dce11::ScanoutSurface& s) {
  if (crtc >= kMaxPipes || (s.address & (kSurfaceAlignment - 1)) != 0 || s.width == 0 || s.height == 0 || s.pitch_pixels < s.width || s.viewport_width == 0 ||
      s.viewport_height == 0 || s.viewport_width > kViewportMax || s.viewport_height > kViewportMax || s.format != amd_dce11::ScanoutFormat::Argb8888 ||
      s.tiling.mode != amd_dce11::ScanoutTiling::Mode::Linear)
    return false;
  const uint32_t dcp = kDcp[crtc], scl = kScl[crtc], lb = kLb[crtc];
  auto update = [&](uint32_t reg, uint32_t mask, uint32_t field) { r_.write(reg, set_field(r_.read(reg), mask, field)); };
  // dce_transform_set_scaler for 1:1: the whole line buffer, no sharpness, no overscan, the scaler bypassed, the viewport
  r_.write(dce(mmLB0_LB_MEMORY_CTRL_BASE_IDX, mmLB0_LB_MEMORY_CTRL) + lb, set_field(set_field(0, LB0_LB_MEMORY_CTRL__LB_MEMORY_CONFIG_MASK, 0), LB0_LB_MEMORY_CTRL__LB_MEMORY_SIZE_MASK, kLbMemorySize));
  r_.write(dce(mmSCL0_SCL_F_SHARP_CONTROL_BASE_IDX, mmSCL0_SCL_F_SHARP_CONTROL) + scl, 0);
  r_.write(dce(mmSCL0_EXT_OVERSCAN_LEFT_RIGHT_BASE_IDX, mmSCL0_EXT_OVERSCAN_LEFT_RIGHT) + scl, 0);
  r_.write(dce(mmSCL0_EXT_OVERSCAN_TOP_BOTTOM_BASE_IDX, mmSCL0_EXT_OVERSCAN_TOP_BOTTOM) + scl, 0);
  r_.write(dce(mmSCL0_SCL_BYPASS_CONTROL_BASE_IDX, mmSCL0_SCL_BYPASS_CONTROL) + scl, set_field(0, SCL0_SCL_BYPASS_CONTROL__SCL_BYPASS_MODE_MASK, 0));
  const uint32_t mode = dce(mmSCL0_SCL_MODE_BASE_IDX, mmSCL0_SCL_MODE) + scl;
  r_.write(mode, set_field(set_field(r_.read(mode), SCL0_SCL_MODE__SCL_MODE_MASK, 0), SCL0_SCL_MODE__SCL_PSCL_EN_MASK, 0));
  r_.write(dce(mmSCL0_VIEWPORT_START_BASE_IDX, mmSCL0_VIEWPORT_START) + scl, set_field(set_field(0, SCL0_VIEWPORT_START__VIEWPORT_X_START_MASK, s.x), SCL0_VIEWPORT_START__VIEWPORT_Y_START_MASK, s.y));
  r_.write(dce(mmSCL0_VIEWPORT_SIZE_BASE_IDX, mmSCL0_VIEWPORT_SIZE) + scl,
           set_field(set_field(0, SCL0_VIEWPORT_SIZE__VIEWPORT_HEIGHT_MASK, s.viewport_height), SCL0_VIEWPORT_SIZE__VIEWPORT_WIDTH_MASK, s.viewport_width));
  update(dce(mmLB0_LB_DATA_FORMAT_BASE_IDX, mmLB0_LB_DATA_FORMAT) + lb, LB0_LB_DATA_FORMAT__ALPHA_EN_MASK, 0);
  // dce_transform_set_pixel_storage_depth for LB_PIXEL_DEPTH_30BPP: the denormalisation, the line buffer's pixel depth and expansion
  update(dce(mmDCP0_DENORM_CONTROL_BASE_IDX, mmDCP0_DENORM_CONTROL) + dcp, DCP0_DENORM_CONTROL__DENORM_MODE_MASK, kDenormMode30Bpp);
  const uint32_t format = dce(mmLB0_LB_DATA_FORMAT_BASE_IDX, mmLB0_LB_DATA_FORMAT) + lb;
  r_.write(format, set_field(set_field(r_.read(format), LB0_LB_DATA_FORMAT__PIXEL_DEPTH_MASK, 0), LB0_LB_DATA_FORMAT__PIXEL_EXPAN_MODE_MASK, 1));
  // dce_mi_program_surface_config: the plane on, GFX9 linear (swizzle mode 0, one pipe, bank, shader engine), the size, ARGB8888 without crossbar swaps
  update(dce(mmDCP0_GRPH_ENABLE_BASE_IDX, mmDCP0_GRPH_ENABLE) + dcp, DCP0_GRPH_ENABLE__GRPH_ENABLE_MASK, 1);
  const uint32_t control = dce(mmDCP0_GRPH_CONTROL_BASE_IDX, mmDCP0_GRPH_CONTROL) + dcp;
  uint32_t c = r_.read(control);
  c = set_field(c, DCP0_GRPH_CONTROL__GRPH_SW_MODE_MASK, 0);
  c = set_field(c, DCP0_GRPH_CONTROL__GRPH_NUM_BANKS_MASK, 0);
  c = set_field(c, DCP0_GRPH_CONTROL__GRPH_NUM_SHADER_ENGINES_MASK, 0);
  c = set_field(c, DCP0_GRPH_CONTROL__GRPH_NUM_PIPES_MASK, 0);
  c = set_field(c, DCP0_GRPH_CONTROL__GRPH_COLOR_EXPANSION_MODE_MASK, 1);
  c = set_field(c, DCP0_GRPH_CONTROL__GRPH_SE_ENABLE_MASK, 0);
  r_.write(control, c);
  r_.write(dce(mmDCP0_GRPH_X_START_BASE_IDX, mmDCP0_GRPH_X_START) + dcp, 0);
  r_.write(dce(mmDCP0_GRPH_Y_START_BASE_IDX, mmDCP0_GRPH_Y_START) + dcp, 0);
  r_.write(dce(mmDCP0_GRPH_X_END_BASE_IDX, mmDCP0_GRPH_X_END) + dcp, s.width);
  r_.write(dce(mmDCP0_GRPH_Y_END_BASE_IDX, mmDCP0_GRPH_Y_END) + dcp, s.height);
  r_.write(dce(mmDCP0_GRPH_PITCH_BASE_IDX, mmDCP0_GRPH_PITCH) + dcp, s.pitch_pixels);
  r_.write(dce(mmDCP0_HW_ROTATION_BASE_IDX, mmDCP0_HW_ROTATION) + dcp, 0);
  r_.write(dce(mmDCP0_GRPH_SWAP_CNTL_BASE_IDX, mmDCP0_GRPH_SWAP_CNTL) + dcp, 0);
  c = r_.read(control);
  c = set_field(c, DCP0_GRPH_CONTROL__GRPH_DEPTH_MASK, 2);
  c = set_field(c, DCP0_GRPH_CONTROL__GRPH_FORMAT_MASK, 0);
  r_.write(control, c);
  const uint32_t prescale = dce(mmDCP0_PRESCALE_GRPH_CONTROL_BASE_IDX, mmDCP0_PRESCALE_GRPH_CONTROL) + dcp;
  r_.write(prescale, r_.read(prescale) & ~(DCP0_PRESCALE_GRPH_CONTROL__GRPH_PRESCALE_SELECT_MASK | DCP0_PRESCALE_GRPH_CONTROL__GRPH_PRESCALE_R_SIGN_MASK |
                                           DCP0_PRESCALE_GRPH_CONTROL__GRPH_PRESCALE_G_SIGN_MASK | DCP0_PRESCALE_GRPH_CONTROL__GRPH_PRESCALE_B_SIGN_MASK));
  // dce_mi_program_surface_flip_and_addr (not immediate: taken at the vertical blank): locked, the high half first, unlocked
  update(dce(mmDCP0_GRPH_UPDATE_BASE_IDX, mmDCP0_GRPH_UPDATE) + dcp, DCP0_GRPH_UPDATE__GRPH_UPDATE_LOCK_MASK, 1);
  update(dce(mmDCP0_GRPH_FLIP_CONTROL_BASE_IDX, mmDCP0_GRPH_FLIP_CONTROL) + dcp, DCP0_GRPH_FLIP_CONTROL__GRPH_SURFACE_UPDATE_H_RETRACE_EN_MASK, 0);
  r_.write(dce(mmDCP0_GRPH_PRIMARY_SURFACE_ADDRESS_HIGH_BASE_IDX, mmDCP0_GRPH_PRIMARY_SURFACE_ADDRESS_HIGH) + dcp,
           set_field(0, DCP0_GRPH_PRIMARY_SURFACE_ADDRESS_HIGH__GRPH_PRIMARY_SURFACE_ADDRESS_HIGH_MASK, static_cast<uint32_t>(s.address >> 32)));
  r_.write(dce(mmDCP0_GRPH_PRIMARY_SURFACE_ADDRESS_BASE_IDX, mmDCP0_GRPH_PRIMARY_SURFACE_ADDRESS) + dcp,
           set_field(0, DCP0_GRPH_PRIMARY_SURFACE_ADDRESS__GRPH_PRIMARY_SURFACE_ADDRESS_MASK, static_cast<uint32_t>(s.address) >> 8));
  update(dce(mmDCP0_GRPH_UPDATE_BASE_IDX, mmDCP0_GRPH_UPDATE) + dcp, DCP0_GRPH_UPDATE__GRPH_UPDATE_LOCK_MASK, 0);
  return true;
}

bool Pipe::disable_surface(uint32_t crtc) {
  if (crtc >= kMaxPipes) return false;
  const uint32_t reg = dce(mmDCP0_GRPH_ENABLE_BASE_IDX, mmDCP0_GRPH_ENABLE) + kDcp[crtc];
  r_.write(reg, set_field(r_.read(reg), DCP0_GRPH_ENABLE__GRPH_ENABLE_MASK, 0));
  return true;
}

bool Pipe::flip_pending(uint32_t crtc) {
  return crtc < kMaxPipes && get_field(r_.read(dce(mmDCP0_GRPH_UPDATE_BASE_IDX, mmDCP0_GRPH_UPDATE) + kDcp[crtc]), DCP0_GRPH_UPDATE__GRPH_SURFACE_UPDATE_PENDING_MASK) != 0;
}

bool Pipe::allocate_dmif(uint32_t crtc, const CrtcMode& m, uint32_t timeout_us) {
  if (crtc >= kMaxPipes) return false;
  const uint32_t reg = dce(2, kDmifBuffer[crtc]);
  const uint32_t control = r_.read(reg);
  if (get_field(control, PIPE0_DMIF_BUFFER_CONTROL__DMIF_BUFFERS_ALLOCATED_MASK) == 2) return true;
  r_.write(reg, set_field(control, PIPE0_DMIF_BUFFER_CONTROL__DMIF_BUFFERS_ALLOCATED_MASK, 2));
  const uint64_t begin = clock_.now_us();
  bool done = false;
  while (!(done = get_field(r_.read(reg), PIPE0_DMIF_BUFFER_CONTROL__DMIF_BUFFERS_ALLOCATION_COMPLETED_MASK) == 1) && clock_.now_us() - begin < timeout_us) {}
  if (m.pixel_clock_khz != 0) {
    const uint32_t duration = static_cast<uint32_t>(1000000000ull / m.pixel_clock_khz);    // (REG_UPDATE keeps the field's low 16 bits)
    const uint32_t arb = dce(mmDMIF_PG0_DPG_PIPE_ARBITRATION_CONTROL1_BASE_IDX, mmDMIF_PG0_DPG_PIPE_ARBITRATION_CONTROL1) + kDmif[crtc];
    r_.write(arb, set_field(r_.read(arb), DMIF_PG0_DPG_PIPE_ARBITRATION_CONTROL1__PIXEL_DURATION_MASK, duration));
  }
  return done;
}

bool Pipe::free_dmif(uint32_t crtc, uint32_t timeout_us) {
  if (crtc >= kMaxPipes) return false;
  const uint32_t reg = dce(2, kDmifBuffer[crtc]);
  const uint32_t control = r_.read(reg);
  if (get_field(control, PIPE0_DMIF_BUFFER_CONTROL__DMIF_BUFFERS_ALLOCATED_MASK) == 0) return true;
  r_.write(reg, set_field(control, PIPE0_DMIF_BUFFER_CONTROL__DMIF_BUFFERS_ALLOCATED_MASK, 0));
  const uint64_t begin = clock_.now_us();
  bool done = false;
  while (!(done = get_field(r_.read(reg), PIPE0_DMIF_BUFFER_CONTROL__DMIF_BUFFERS_ALLOCATION_COMPLETED_MASK) == 1) && clock_.now_us() - begin < timeout_us) {}
  return done;
}

bool Pipe::safe_watermarks(uint32_t crtc) {
  if (crtc >= kMaxPipes) return false;
  const uint32_t off = kDmif[crtc];
  const uint32_t mask = dce(mmDMIF_PG0_DPG_WATERMARK_MASK_CONTROL_BASE_IDX, mmDMIF_PG0_DPG_WATERMARK_MASK_CONTROL) + off;
  auto update = [&](uint32_t reg, uint32_t field_mask, uint32_t value) { r_.write(reg, set_field(r_.read(reg), field_mask, value)); };
  auto marks = [&](uint32_t low, uint32_t high, uint32_t low_mask, uint32_t high_mask) { return set_field(set_field(0, low_mask, low), high_mask, high); };
  // dce120_program_urgency_watermark, sets a..d: the urgent marks, the high one the line time (MAX_WATERMARK)
  for (uint32_t set = 0; set < 4; ++set) {
    update(mask, DMIF_PG0_DPG_WATERMARK_MASK_CONTROL__URGENCY_WATERMARK_MASK_MASK, set);
    r_.write(dce(mmDMIF_PG0_DPG_PIPE_URGENCY_CONTROL_BASE_IDX, mmDMIF_PG0_DPG_PIPE_URGENCY_CONTROL) + off, marks(kMaxWatermark, kMaxWatermark, DMIF_PG0_DPG_PIPE_URGENCY_CONTROL__URGENCY_LOW_WATERMARK_MASK,
                                                                     DMIF_PG0_DPG_PIPE_URGENCY_CONTROL__URGENCY_HIGH_WATERMARK_MASK));
    r_.write(dce(mmDMIF_PG0_DPG_PIPE_URGENT_LEVEL_CONTROL_BASE_IDX, mmDMIF_PG0_DPG_PIPE_URGENT_LEVEL_CONTROL) + off, marks(kMaxWatermark, kMaxWatermark, DMIF_PG0_DPG_PIPE_URGENT_LEVEL_CONTROL__URGENT_LEVEL_LOW_WATERMARK_MASK,
                                                                         DMIF_PG0_DPG_PIPE_URGENT_LEVEL_CONTROL__URGENT_LEVEL_HIGH_WATERMARK_MASK));
  }
  const uint32_t stutter = dce(mmDMIF_PG0_DPG_PIPE_STUTTER_CONTROL_BASE_IDX, mmDMIF_PG0_DPG_PIPE_STUTTER_CONTROL) + off;
  r_.write(stutter, set_field(set_field(r_.read(stutter), DMIF_PG0_DPG_PIPE_STUTTER_CONTROL__STUTTER_ENABLE_MASK, 1), DMIF_PG0_DPG_PIPE_STUTTER_CONTROL__STUTTER_IGNORE_FBC_MASK, 1));
  // program_nbp_watermark: DCE 12 has no NB P-state register, only the low power control's P-state change
  const uint32_t power = dce(mmDMIF_PG0_DPG_PIPE_LOW_POWER_CONTROL_BASE_IDX, mmDMIF_PG0_DPG_PIPE_LOW_POWER_CONTROL) + off;
  for (uint32_t set = 0; set < 4; ++set) {
    update(mask, DMIF_PG0_DPG_WATERMARK_MASK_CONTROL__PSTATE_CHANGE_WATERMARK_MASK_MASK, set);
    uint32_t v = r_.read(power);
    v = set_field(v, DMIF_PG0_DPG_PIPE_LOW_POWER_CONTROL__PSTATE_CHANGE_ENABLE_MASK, 1);
    v = set_field(v, DMIF_PG0_DPG_PIPE_LOW_POWER_CONTROL__PSTATE_CHANGE_URGENT_DURING_REQUEST_MASK, 1);
    v = set_field(v, DMIF_PG0_DPG_PIPE_LOW_POWER_CONTROL__PSTATE_CHANGE_NOT_SELF_REFRESH_DURING_REQUEST_MASK, 1);
    r_.write(power, v);
    update(power, DMIF_PG0_DPG_PIPE_LOW_POWER_CONTROL__PSTATE_CHANGE_WATERMARK_MASK, kSafeNbpMark);
  }
  // dce120_program_stutter_watermark: exit MAX_WATERMARK, entry 0
  const uint32_t stutter2 = dce(mmDMIF_PG0_DPG_PIPE_STUTTER_CONTROL2_BASE_IDX, mmDMIF_PG0_DPG_PIPE_STUTTER_CONTROL2) + off;
  for (uint32_t set = 0; set < 4; ++set) {
    update(mask, DMIF_PG0_DPG_WATERMARK_MASK_CONTROL__STUTTER_EXIT_SELF_REFRESH_WATERMARK_MASK_MASK, set);
    uint32_t v = r_.read(stutter2);
    v = set_field(v, DMIF_PG0_DPG_PIPE_STUTTER_CONTROL2__STUTTER_EXIT_SELF_REFRESH_WATERMARK_MASK, kMaxWatermark);
    v = set_field(v, DMIF_PG0_DPG_PIPE_STUTTER_CONTROL2__STUTTER_ENTER_SELF_REFRESH_WATERMARK_MASK, 0);
    r_.write(stutter2, v);
  }
  return true;
}

bool Pipe::connect(uint32_t dig, uint32_t crtc, bool on) {
  if (dig >= kMaxDigs || crtc >= kMaxPipes) return false;
  const uint32_t fe = dce(mmDIG0_DIG_FE_CNTL_BASE_IDX, mmDIG0_DIG_FE_CNTL) + kDig[dig], be = dce(mmDIG0_DIG_BE_CNTL_BASE_IDX, mmDIG0_DIG_BE_CNTL) + kDig[dig];
  if (on) r_.write(fe, set_field(r_.read(fe), DIG0_DIG_FE_CNTL__DIG_SOURCE_SELECT_MASK, crtc));
  const uint32_t v = r_.read(be);
  uint32_t sources = get_field(v, DIG0_DIG_BE_CNTL__DIG_FE_SOURCE_SELECT_MASK);
  sources = on ? (sources | (1u << dig)) : (sources & ~(1u << dig));      // get_frontend_source: DIGA 0x1, DIGB 0x2, ...
  r_.write(be, set_field(v, DIG0_DIG_BE_CNTL__DIG_FE_SOURCE_SELECT_MASK, sources));
  return true;
}

bool Pipe::set_hpd_select(uint32_t dig, uint32_t hpd_source) {
  if (dig >= kMaxDigs) return false;
  const uint32_t be = dce(mmDIG0_DIG_BE_CNTL_BASE_IDX, mmDIG0_DIG_BE_CNTL) + kDig[dig];
  r_.write(be, set_field(r_.read(be), DIG0_DIG_BE_CNTL__DIG_HPD_SELECT_MASK, hpd_source));
  return true;
}

bool Pipe::hpd_enable(uint32_t pin, bool enable) {
  if (pin >= kMaxHpd) return false;
  const uint32_t reg = dce(mmHPD0_DC_HPD_CONTROL_BASE_IDX, mmHPD0_DC_HPD_CONTROL) + kHpd[pin];
  r_.write(reg, set_field(r_.read(reg), HPD0_DC_HPD_CONTROL__DC_HPD_EN_MASK, enable ? 1 : 0));
  return true;
}

// ------------------------------------------------------------------------------------------------------------------------- command tables

bool CommandTables::revision(uint32_t table, uint8_t want) {
  uint8_t frev = 0, crev = 0;
  return bios_.cmd_table_header(table, &frev, &crev) && crev == want;     // (BIOS_CMD_TABLE_PARA_REVISION: the content revision)
}

bool CommandTables::run(uint32_t table, uint32_t* words, uint32_t count) {
  return bios_.execute_table(table, ArrayView<uint32_t>(words, count)) == AtomBios::Status::Ok;
}

bool CommandTables::set_pixel_clock(uint8_t crtc, uint8_t pll, uint8_t encoder_object_id, uint8_t encoder_mode, uint32_t pixel_clock_100hz, uint8_t misc) {
  if (!revision(kSetPixelClock, 7)) return false;
  uint32_t words[4] = {pixel_clock_100hz, uint32_t{pll} | (uint32_t{encoder_object_id} << 8) | (uint32_t{encoder_mode} << 16) | (uint32_t{misc} << 24), uint32_t{crtc}, 0};
  return run(kSetPixelClock, words, 4);
}

bool CommandTables::set_crtc_timing(uint8_t crtc, const CrtcMode& m) {
  if (!revision(kSetCrtcUsingDtdTiming, 3) || m.interlaced || m.double_scan) return false;
  if (m.h_total < m.h_display || m.v_total < m.v_display || m.h_sync_start < m.h_display || m.v_sync_start < m.v_display || m.h_sync_end < m.h_sync_start ||
      m.v_sync_end < m.v_sync_start)
    return false;
  constexpr uint32_t kHSyncPolarity = 0x02, kVSyncPolarity = 0x04;      // ATOM_HSYNC_POLARITY, ATOM_VSYNC_POLARITY: set for a negative sync
  uint32_t misc = 0;
  if (m.h_sync_negative) misc |= kHSyncPolarity;
  if (m.v_sync_negative) misc |= kVSyncPolarity;
  uint32_t words[6] = {uint32_t{m.h_display} | (uint32_t(m.h_total - m.h_display) << 16), uint32_t{m.v_display} | (uint32_t(m.v_total - m.v_display) << 16),
                       uint32_t(m.h_sync_start - m.h_display) | (uint32_t(m.h_sync_end - m.h_sync_start) << 16),
                       uint32_t(m.v_sync_start - m.v_display) | (uint32_t(m.v_sync_end - m.v_sync_start) << 16), misc, uint32_t{crtc}};
  return run(kSetCrtcUsingDtdTiming, words, 6);
}

bool CommandTables::enable_crtc(uint8_t crtc, bool enable) {
  if (!revision(kEnableCrtc, 1)) return false;
  uint32_t words[1] = {uint32_t{crtc} | (uint32_t{enable ? 1u : 0u} << 8)};
  return run(kEnableCrtc, words, 1);
}

bool CommandTables::transmitter(const Transmitter& t) {
  if (!revision(kTransmitterControl, 6) || t.phy >= kMaxDigs) return false;
  uint32_t words[4] = {uint32_t{t.phy} | (uint32_t{t.action} << 8) | (uint32_t{t.mode_or_lane_set} << 16) | (uint32_t{t.lanes} << 24), t.symbol_clock_khz / 10,
                       uint32_t{t.hpd_select} | (0u << 8) | (uint32_t{t.connector_object_id} << 16), 0};
  return run(kTransmitterControl, words, 4);
}

bool CommandTables::encoder_stream_setup(uint8_t dig, uint8_t encoder_mode, uint8_t lanes, uint32_t pixel_clock_khz, uint8_t panel_bpc, uint32_t link_rate_khz) {
  constexpr uint8_t kStreamSetup = 0x0F;     // ATOM_ENCODER_CMD_STREAM_SETUP
  if (!revision(kDigEncoderControl, 5) || dig >= kMaxDigs) return false;
  uint32_t words[3] = {uint32_t{dig} | (uint32_t{kStreamSetup} << 8) | (uint32_t{encoder_mode} << 16) | (uint32_t{lanes} << 24), pixel_clock_khz / 10,
                       uint32_t{panel_bpc} | ((link_rate_khz / 27000) << 8)};
  return run(kDigEncoderControl, words, 3);
}

bool CommandTables::encoder_panel_mode(uint8_t dig, uint8_t panel_mode) {
  constexpr uint8_t kSetupPanelMode = 0x10;  // ATOM_ENCODER_CMD_SETUP_PANEL_MODE
  if (!revision(kDigEncoderControl, 5) || dig >= kMaxDigs) return false;
  uint32_t words[3] = {uint32_t{dig} | (uint32_t{kSetupPanelMode} << 8) | (uint32_t{panel_mode} << 16), 0, 0};
  return run(kDigEncoderControl, words, 3);
}

bool CommandTables::encoder_command(uint8_t dig, uint8_t action) {
  if (!revision(kDigEncoderControl, 5) || dig >= kMaxDigs) return false;
  uint32_t words[3] = {uint32_t{dig} | (uint32_t{action} << 8), 0, 0};
  return run(kDigEncoderControl, words, 3);
}

// ------------------------------------------------------------------------------------------------------------------------- DP source

bool DpSource::set_pattern(uint8_t pattern) {
  switch (pattern) {     // ATOM_ENCODER_CMD_DP_LINK_TRAINING_PATTERN1 / 2 / 3
    case Dpcd::kTrainingPattern1: return tables_.encoder_command(dig_, 0x09);
    case Dpcd::kTrainingPattern2: return tables_.encoder_command(dig_, 0x0A);
    case Dpcd::kTrainingPattern3: return tables_.encoder_command(dig_, 0x13);
    default: return false;
  }
}

bool DpSource::set_drive(uint8_t lane_set) {
  constexpr uint8_t kSetupVsEmph = 11;       // ATOM_TRANSMITTER_ACTION_SETUP_VSEMPH (TRANSMITTER_CONTROL_SET_VOLTAGE_AND_PREEMPASIS)
  CommandTables::Transmitter t = base_;
  t.action = kSetupVsEmph;
  t.mode_or_lane_set = lane_set;
  bool ok = true;
  for (uint32_t lane = 0; lane < base_.lanes; ++lane) ok = tables_.transmitter(t) && ok;
  return ok;
}

}  // namespace amd_dce12
}  // namespace hardware
