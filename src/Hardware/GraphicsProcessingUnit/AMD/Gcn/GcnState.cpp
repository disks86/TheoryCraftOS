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

#include "Hardware/GraphicsProcessingUnit/AMD/Gcn/GcnState.hpp"

namespace gcn {

using namespace pm4;

namespace {

constexpr uint32_t kMaxAddressBits = 40;
// the registers an SGPR count includes beyond the shader's own: VCC (2), FLAT_SCRATCH (2) and XNACK_MASK (2) on VI
constexpr uint32_t kReservedSgprs = 6, kMaxSgprs = 102, kMaxVgprs = 256;
// SPI_SHADER_PGM_RSRC1 FLOAT_MODE: single precision flushes denormals, half and double allow them (the emulator flushes)
constexpr uint32_t kFloatMode = 0xC0;
constexpr uint32_t kMaxInterpolants = 16;
constexpr uint32_t kMaxTargetDimension = 16384;
constexpr uint32_t kNumFormatFloat = 7;                    // the V# NUM_FORMAT of floats (BUF_NUM_FORMAT_FLOAT)
constexpr uint32_t kDataFormat[4] = {4, 11, 13, 14};       // BUF_DATA_FORMAT_32, _32_32, _32_32_32, _32_32_32_32
constexpr uint32_t kBufferWordShift = 12;                  // NUM_FORMAT at bit 12 of dword 3, DATA_FORMAT at bit 15

uint32_t float_bits(float f) {
  union { float f; uint32_t u; } c;
  c.f = f;
  return c.u;
}

bool fits(uint64_t address) { return (address >> kMaxAddressBits) == 0; }

bool stage_ok(const ShaderProgram& p, bool fragment) {
  const ShaderInfo& i = p.info;
  return i.fragment == fragment && (p.address & 0xFF) == 0 && p.address != 0 && fits(p.address) && i.vgprs <= kMaxVgprs &&
         i.sgprs <= kMaxSgprs && i.user_sgprs == abi::kUserSgprs;
}

}  // namespace

uint32_t shader_resource1(const ShaderInfo& info) {
  const uint32_t vgpr_granules = (info.vgprs == 0 ? 0 : (info.vgprs - 1) / 4);
  const uint32_t sgpr_granules = (info.sgprs + kReservedSgprs + 7) / 8 - 1;
  return (vgpr_granules << SPI_SHADER_PGM_RSRC1_VS__VGPRS__SHIFT) | (sgpr_granules << SPI_SHADER_PGM_RSRC1_VS__SGPRS__SHIFT) | (kFloatMode << SPI_SHADER_PGM_RSRC1_VS__FLOAT_MODE__SHIFT) |
         (1u << SPI_SHADER_PGM_RSRC1_VS__DX10_CLAMP__SHIFT);
}

uint32_t shader_resource2(const ShaderInfo& info) { return info.user_sgprs << SPI_SHADER_PGM_RSRC2_VS__USER_SGPR__SHIFT; }

bool make_vertex_descriptor(ArrayView<uint32_t> out, uint64_t base, uint32_t stride, uint32_t num_records, uint32_t components) {
  if (out.size() < 4 || components < 1 || components > 4 || stride == 0 || stride > 16383 || !fits(base) || (base & 3) != 0) return false;
  out[0] = static_cast<uint32_t>(base);
  out[1] = static_cast<uint32_t>(base >> 32) | (stride << 16);
  out[2] = num_records;
  out[3] = (kNumFormatFloat << kBufferWordShift) | (kDataFormat[components - 1] << (kBufferWordShift + 3));
  return true;
}

bool emit_draw(Pm4Writer& w, const DrawState& s) {
  const ColorTarget& t = s.target;
  const ShaderInfo& vi = s.vertex.info;
  const ShaderInfo& pi = s.pixel.info;
  const bool indexed = s.index_address != 0;
  const bool ok = stage_ok(s.vertex, false) && stage_ok(s.pixel, true) && fits(s.vertex_constants) && fits(s.pixel_constants) && fits(s.descriptor_table) && (s.descriptor_table & 3) == 0 &&
                  (s.vertex_constants & 3) == 0 && (s.pixel_constants & 3) == 0 && t.width != 0 && t.height != 0 && t.width <= kMaxTargetDimension && t.height <= kMaxTargetDimension &&
                  t.pitch_pixels >= t.width && t.pitch_pixels % 8 == 0 && t.pitch_pixels <= kMaxTargetDimension && (t.address & 0xFF) == 0 && fits(t.address) && t.address != 0 &&
                  pi.ps_inputs <= kMaxInterpolants && vi.param_exports <= kMaxInterpolants && s.index_count != 0 && (!indexed || (fits(s.index_address) && s.index_max >= s.index_count)) &&
                  (s.fence_address == 0 || ((s.fence_address & 7) == 0 && fits(s.fence_address)));
  if (!ok) return false;
  for (uint32_t k = 0; k < pi.ps_inputs; ++k)
    if (s.ps_param[k] >= kMaxInterpolants) return false;

  // the shaders: the code, the resources, the user data (constants, descriptor table) of each stage
  const uint32_t vs_regs[] = {static_cast<uint32_t>(s.vertex.address >> 8), static_cast<uint32_t>(s.vertex.address >> 40), shader_resource1(vi), shader_resource2(vi)};
  const uint32_t ps_regs[] = {static_cast<uint32_t>(s.pixel.address >> 8), static_cast<uint32_t>(s.pixel.address >> 40), shader_resource1(pi), shader_resource2(pi)};
  w.set_sh_reg(mmSPI_SHADER_PGM_LO_VS, ArrayView<const uint32_t>(vs_regs, 4));
  const uint32_t vs_user[] = {static_cast<uint32_t>(s.vertex_constants), static_cast<uint32_t>(s.vertex_constants >> 32), static_cast<uint32_t>(s.descriptor_table),
                              static_cast<uint32_t>(s.descriptor_table >> 32)};
  w.set_sh_reg(mmSPI_SHADER_USER_DATA_VS_0, ArrayView<const uint32_t>(vs_user, 4));
  w.set_sh_reg(mmSPI_SHADER_PGM_LO_PS, ArrayView<const uint32_t>(ps_regs, 4));
  const uint32_t ps_user[] = {static_cast<uint32_t>(s.pixel_constants), static_cast<uint32_t>(s.pixel_constants >> 32), static_cast<uint32_t>(s.descriptor_table),
                             static_cast<uint32_t>(s.descriptor_table >> 32)};
  w.set_sh_reg(mmSPI_SHADER_USER_DATA_PS_0, ArrayView<const uint32_t>(ps_user, 4));

  // the stages: the vertex shader only (no LS, HS, ES or GS)
  w.set_context_reg(mmVGT_SHADER_STAGES_EN, 0);
  w.set_context_reg(mmVGT_OUTPUT_PATH_CNTL, 0);
  w.set_context_reg(mmVGT_MIN_VTX_INDX, 0);
  w.set_context_reg(mmVGT_MAX_VTX_INDX, 0xFFFFFFFFu);

  // what the shaders export and take: the position (4 components), the colour as 32 bit floats; the parameters and the interpolants
  w.set_context_reg(mmSPI_SHADER_POS_FORMAT, kSPI_SHADER_4COMP << SPI_SHADER_POS_FORMAT__POS0_EXPORT_FORMAT__SHIFT);
  w.set_context_reg(mmSPI_SHADER_Z_FORMAT, kSPI_SHADER_ZERO);
  w.set_context_reg(mmSPI_SHADER_COL_FORMAT, kSPI_SHADER_32_ABGR << SPI_SHADER_COL_FORMAT__COL0_EXPORT_FORMAT__SHIFT);
  w.set_context_reg(mmSPI_VS_OUT_CONFIG, (vi.param_exports == 0 ? 0 : vi.param_exports - 1) << SPI_VS_OUT_CONFIG__VS_EXPORT_COUNT__SHIFT);
  w.set_context_reg(mmPA_CL_VS_OUT_CNTL, 0);
  w.set_context_reg(mmSPI_PS_INPUT_ENA, pi.ps_input_enable);
  w.set_context_reg(mmSPI_PS_INPUT_ADDR, pi.ps_input_enable);
  w.set_context_reg(mmSPI_PS_IN_CONTROL, pi.ps_inputs << SPI_PS_IN_CONTROL__NUM_INTERP__SHIFT);
  if (pi.ps_inputs != 0) {
    uint32_t cntl[kMaxInterpolants];
    for (uint32_t k = 0; k < pi.ps_inputs; ++k) cntl[k] = static_cast<uint32_t>(s.ps_param[k]) << SPI_PS_INPUT_CNTL_0__OFFSET__SHIFT;
    w.set_context_reg(mmSPI_PS_INPUT_CNTL_0, ArrayView<const uint32_t>(cntl, pi.ps_inputs));
  }

  // the colour buffer: base and pitch in units of 256 bytes and 8 pixels, linear, float 32_32_32_32, no blending
  const uint32_t pitch_tiles = t.pitch_pixels / 8 - 1;
  const uint32_t slice_tiles = t.pitch_pixels * ((t.height + 7) / 8 * 8) / 64 - 1;           // (the slice is whole rows of 8 by 8 pixel tiles)
  const uint32_t info = (kENDIAN_NONE << CB_COLOR0_INFO__ENDIAN__SHIFT) | (kCOLOR_32_32_32_32 << CB_COLOR0_INFO__FORMAT__SHIFT) | (kNUMBER_FLOAT << CB_COLOR0_INFO__NUMBER_TYPE__SHIFT) |
                       (kSWAP_STD << CB_COLOR0_INFO__COMP_SWAP__SHIFT) | (1u << CB_COLOR0_INFO__BLEND_BYPASS__SHIFT) | (1u << CB_COLOR0_INFO__SIMPLE_FLOAT__SHIFT) |
                       (1u << CB_COLOR0_INFO__ROUND_MODE__SHIFT);
  const uint32_t color[] = {static_cast<uint32_t>(t.address >> 8), pitch_tiles << CB_COLOR0_PITCH__TILE_MAX__SHIFT, slice_tiles << CB_COLOR0_SLICE__TILE_MAX__SHIFT, 0, info,
                            ColorTarget::kLinearAlignedTileIndex << CB_COLOR0_ATTRIB__TILE_MODE_INDEX__SHIFT};
  w.set_context_reg(mmCB_COLOR0_BASE, ArrayView<const uint32_t>(color, 6));
  w.set_context_reg(mmCB_TARGET_MASK, 0xF);
  w.set_context_reg(mmCB_SHADER_MASK, 0xF);
  w.set_context_reg(mmCB_COLOR_CONTROL, (kCB_NORMAL << CB_COLOR_CONTROL__MODE__SHIFT) | (0xCCu << CB_COLOR_CONTROL__ROP3__SHIFT));
  w.set_context_reg(mmCB_BLEND0_CONTROL, 0);
  // no depth, stencil or alpha test
  w.set_context_reg(mmDB_DEPTH_CONTROL, 0);
  w.set_context_reg(mmDB_RENDER_CONTROL, 0);
  w.set_context_reg(mmDB_SHADER_CONTROL, 0);
  w.set_context_reg(mmDB_Z_INFO, 0);
  w.set_context_reg(mmDB_STENCIL_INFO, 0);

  // the viewport transform over the target (y down), scissors over the target, no clipping modes beyond the defaults, pixel centres at the half
  const uint32_t viewport[] = {float_bits(0.5f * static_cast<float>(t.width)), float_bits(0.5f * static_cast<float>(t.width)), float_bits(-0.5f * static_cast<float>(t.height)),
                               float_bits(0.5f * static_cast<float>(t.height)), float_bits(0.5f), float_bits(0.5f)};
  w.set_context_reg(mmPA_CL_VPORT_XSCALE, ArrayView<const uint32_t>(viewport, 6));
  const uint32_t vte = (1u << PA_CL_VTE_CNTL__VPORT_X_SCALE_ENA__SHIFT) | (1u << PA_CL_VTE_CNTL__VPORT_X_OFFSET_ENA__SHIFT) | (1u << PA_CL_VTE_CNTL__VPORT_Y_SCALE_ENA__SHIFT) |
                       (1u << PA_CL_VTE_CNTL__VPORT_Y_OFFSET_ENA__SHIFT) | (1u << PA_CL_VTE_CNTL__VPORT_Z_SCALE_ENA__SHIFT) | (1u << PA_CL_VTE_CNTL__VPORT_Z_OFFSET_ENA__SHIFT) |
                       (1u << PA_CL_VTE_CNTL__VTX_W0_FMT__SHIFT);
  w.set_context_reg(mmPA_CL_VTE_CNTL, vte);
  w.set_context_reg(mmPA_CL_CLIP_CNTL, 0);
  w.set_context_reg(mmPA_SU_SC_MODE_CNTL, 0);                                           // no culling, filled
  w.set_context_reg(mmPA_SU_VTX_CNTL, 1u << PA_SU_VTX_CNTL__PIX_CENTER__SHIFT);
  w.set_context_reg(mmPA_SC_WINDOW_OFFSET, 0);
  const uint32_t scissor_tl = 1u << PA_SC_WINDOW_SCISSOR_TL__WINDOW_OFFSET_DISABLE__SHIFT;
  const uint32_t scissor_br = (t.width << PA_SC_WINDOW_SCISSOR_BR__BR_X__SHIFT) | (t.height << PA_SC_WINDOW_SCISSOR_BR__BR_Y__SHIFT);
  w.set_context_reg(mmPA_SC_SCREEN_SCISSOR_TL, 0);
  w.set_context_reg(mmPA_SC_SCREEN_SCISSOR_BR, scissor_br);
  w.set_context_reg(mmPA_SC_WINDOW_SCISSOR_TL, scissor_tl);
  w.set_context_reg(mmPA_SC_WINDOW_SCISSOR_BR, scissor_br);
  w.set_context_reg(mmPA_SC_GENERIC_SCISSOR_TL, scissor_tl);
  w.set_context_reg(mmPA_SC_GENERIC_SCISSOR_BR, scissor_br);
  w.set_context_reg(mmPA_SC_VPORT_SCISSOR_0_TL, scissor_tl);
  w.set_context_reg(mmPA_SC_VPORT_SCISSOR_0_BR, scissor_br);
  const uint32_t zrange[] = {float_bits(0.0f), float_bits(1.0f)};
  w.set_context_reg(mmPA_SC_VPORT_ZMIN_0, ArrayView<const uint32_t>(zrange, 2));

  // the primitives and the draw
  w.set_uconfig_reg(mmVGT_PRIMITIVE_TYPE, kDI_PT_TRILIST);
  w.num_instances(1);
  if (indexed) {
    w.index_type(kDI_INDEX_SIZE_32_BIT);
    w.draw_index_2(s.index_max, s.index_address, s.index_count);
  } else {
    w.draw_index_auto(s.index_count);
  }
  // the colour buffer's caches written back, then the fence
  w.event_write(kPS_PARTIAL_FLUSH, 4);
  w.acquire_mem(kCoherCbActionEna | kCoherDbActionEna | kCoherTcActionEna | kCoherTcl1ActionEna);
  if (s.fence_address != 0) w.event_write_eop(kBOTTOM_OF_PIPE_TS, kCoherTcWbActionEna | kCoherTcActionEna, s.fence_address, s.fence_value, 2, 0);
  return w.ok();
}

}  // namespace gcn
