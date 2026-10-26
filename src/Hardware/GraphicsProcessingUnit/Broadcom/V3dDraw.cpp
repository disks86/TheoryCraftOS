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

#include "Hardware/GraphicsProcessingUnit/Broadcom/V3dDraw.hpp"

namespace hardware {

using namespace v3d_pkt;

namespace {
constexpr uint32_t kAttributeFloat = 2;
constexpr uint32_t kPrimitiveTriangles = 4;
constexpr uint32_t kVpmSegmentSize = 1;  // sectors of 8 rows: a vertex of up to 8 values fits one (the shaders read at most 7 and write at most 8)
constexpr uint32_t kBlendEnableRt0 = 1;

constexpr uint32_t kOneBits = 0x3F800000;  // 1.0f

void copy_words(ArrayView<uint8_t> dest, ArrayView<const uint32_t> words) {
  for (uint64_t i = 0; i < words.size(); ++i)
    for (uint32_t b = 0; b < 4; ++b) dest[i * 4 + b] = static_cast<uint8_t>(words[i] >> (8 * b));
}

// Appends a uniform stream (16-byte aligned) and returns its GPU address; 0 with the list marked bad when it does not fit.
uint32_t emit_uniforms(V3dControlList& indirect, ArrayView<const uint32_t> words) {
  indirect.align(16);
  const uint32_t address = indirect.gpu_end();
  ArrayView<uint8_t> bytes = indirect.append(static_cast<uint32_t>(words.size() * 4));
  if (!indirect.ok()) return 0;
  copy_words(bytes, words);
  return address;
}
}  // namespace

void V3dDrawEmitter::fill_attribute_defaults(ArrayView<uint32_t> out) {
  for (uint64_t i = 0; i < out.size(); ++i) out[i] = (i % 4 == 3) ? kOneBits : 0;
}

bool V3dDrawEmitter::emit_state(V3dControlList& bcl, const V3dFrameDesc& frame, const V3dViewport& vp, const V3dDrawState& st, const V3dClipRect* clip) {
  if (vp.width == 0 || vp.height == 0 || vp.x > frame.width || vp.width > frame.width - vp.x || vp.y > frame.height || vp.height > frame.height - vp.y || vp.width > 16000 || vp.height > 16000)
    return false;
  const bool depth = st.depth_test && frame.has_depth;
  uint32_t min_x = vp.x, min_y = vp.y, max_x = vp.x + vp.width, max_y = vp.y + vp.height;
  if (clip) {
    if (clip->width == 0 || clip->height == 0 || clip->x > frame.width || clip->width > frame.width - clip->x || clip->y > frame.height || clip->height > frame.height - clip->y) return false;
    if (clip->x > min_x) min_x = clip->x;
    if (clip->y > min_y) min_y = clip->y;
    if (clip->x + clip->width < max_x) max_x = clip->x + clip->width;
    if (clip->y + clip->height < max_y) max_y = clip->y + clip->height;
    if (min_x >= max_x || min_y >= max_y) return false;
  }
  {
    V3dPacketBuilder p = bcl.emit(clip_window::kInfo);
    p.set(clip_window::kClipWindowLeftPixelCoordinate, min_x).set(clip_window::kClipWindowBottomPixelCoordinate, min_y);
    p.set(clip_window::kClipWindowWidthInPixels, max_x - min_x).set(clip_window::kClipWindowHeightInPixels, max_y - min_y);
    if (!p.ok()) bcl.mark_bad();
  }
  {
    V3dPacketBuilder p = bcl.emit(cfg_bits::kInfo);
    p.set(cfg_bits::kEnableForwardFacingPrimitive, st.cull != V3dDrawState::Cull::Front);
    p.set(cfg_bits::kEnableReverseFacingPrimitive, st.cull != V3dDrawState::Cull::Back);
    p.set(cfg_bits::kClockwisePrimitives, st.front_clockwise);  // the hardware's clockwise is that of window coordinates with row 0 first, as the screen
    p.set(cfg_bits::kBlendEnable, st.alpha_blend ? kBlendEnableRt0 : 0);
    p.set(cfg_bits::kZUpdatesEnable, depth && st.depth_write);
    p.set(cfg_bits::kDepthTestFunction, depth ? compare_function::kLEQUAL : compare_function::kALWAYS);
    p.set(cfg_bits::kLineRasterization, line_rasterization::kDiamondExit);
    if (!p.ok()) bcl.mark_bad();
  }
  {
    V3dPacketBuilder p = bcl.emit(point_size::kInfo);
    p.set(point_size::kPointSize, kOneBits);
    if (!p.ok()) bcl.mark_bad();
  }
  {
    V3dPacketBuilder p = bcl.emit(line_width::kInfo);
    p.set(line_width::kLineWidth, kOneBits);
    if (!p.ok()) bcl.mark_bad();
  }
  {
    V3dPacketBuilder p = bcl.emit(clipper_xy_scaling::kInfo);  // half the viewport in 1/256 pixel
    p.set(clipper_xy_scaling::kViewportHalfWidthIn1256thOfPixel, v3d_shader::float_bits(vp.width * 128)).set(clipper_xy_scaling::kViewportHalfHeightIn1256thOfPixel, v3d_shader::float_bits(vp.height * 128));
    if (!p.ok()) bcl.mark_bad();
  }
  {
    V3dPacketBuilder p = bcl.emit(clipper_z_scale_and_offset::kInfo);
    p.set(clipper_z_scale_and_offset::kViewportZScaleZcToZs, vp.z_scale_bits).set(clipper_z_scale_and_offset::kViewportZOffsetZcToZs, vp.z_offset_bits);
    if (!p.ok()) bcl.mark_bad();
  }
  {
    V3dPacketBuilder p = bcl.emit(clipper_z_min_max_clipping_planes::kInfo);  // depth runs 0..1 in clip space: the window z range is [offset, offset + scale]
    p.set(clipper_z_min_max_clipping_planes::kMinimumZw, vp.z_offset_bits).set(clipper_z_min_max_clipping_planes::kMaximumZw, vp.z_max_bits);
    if (!p.ok()) bcl.mark_bad();
  }
  {
    V3dPacketBuilder p = bcl.emit(viewport_offset::kInfo);  // the window position of the viewport centre, in 1/256 pixel (unsigned: it is not negative here)
    p.set(viewport_offset::kFineX, uint64_t{vp.x * 2 + vp.width} * 128).set(viewport_offset::kFineY, uint64_t{vp.y * 2 + vp.height} * 128);
    if (!p.ok()) bcl.mark_bad();
  }
  if (st.alpha_blend) {
    {
      V3dPacketBuilder p = bcl.emit(blend_enables::kInfo);
      p.set(blend_enables::kMask, kBlendEnableRt0);
      if (!p.ok()) bcl.mark_bad();
    }
    V3dPacketBuilder p = bcl.emit(blend_cfg::kInfo);  // colour: src alpha, 1 - src alpha; alpha: 1, 1 - src alpha
    p.set(blend_cfg::kRenderTargetMask, 0xF);
    p.set(blend_cfg::kColorBlendMode, blend_mode::kADD).set(blend_cfg::kColorBlendSrcFactor, blend_factor::kSRCALPHA).set(blend_cfg::kColorBlendDstFactor, blend_factor::kINVSRCALPHA);
    p.set(blend_cfg::kAlphaBlendMode, blend_mode::kADD).set(blend_cfg::kAlphaBlendSrcFactor, blend_factor::kONE).set(blend_cfg::kAlphaBlendDstFactor, blend_factor::kINVSRCALPHA);
    if (!p.ok()) bcl.mark_bad();
  }
  {
    V3dPacketBuilder p = bcl.emit(color_write_masks::kInfo);
    p.set(color_write_masks::kMask, 0xFFFFFFFF);
    if (!p.ok()) bcl.mark_bad();
  }
  bcl.emit_simple(zero_all_flat_shade_flags::kInfo);
  bcl.emit_simple(zero_all_non_perspective_flags::kInfo);
  bcl.emit_simple(zero_all_centroid_flags::kInfo);
  {
    V3dPacketBuilder p = bcl.emit(transform_feedback_specs::kInfo);  // transform feedback off
    p.set(transform_feedback_specs::kEnable, false);
    if (!p.ok()) bcl.mark_bad();
  }
  {
    V3dPacketBuilder p = bcl.emit(sample_state::kInfo);
    p.set(sample_state::kCoverage, 0x3F80);  // 1.0 as a bfloat16
    p.set(sample_state::kMask, 0xF);
    if (!p.ok()) bcl.mark_bad();
  }
  return bcl.ok();
}

bool V3dDrawEmitter::emit_draw(V3dControlList& bcl, V3dControlList& indirect, const V3dDrawCall& d) {
  if (d.attribute_count == 0 || d.attribute_count > 4 || d.vertex_count == 0 || d.defaults_address == 0 || (d.defaults_address & 15) || (d.code_address & 7)) return false;
  if (d.indexed && (d.index_buffer_bytes == 0 || (d.index_bytes != 1 && d.index_bytes != 2 && d.index_bytes != 4))) return false;
  if (d.coordinate_uniforms.size() < v3d_shader::kCoordinateUniformWords || d.vertex_uniforms.size() < v3d_shader::kVertexUniformWords ||
      d.fragment_uniforms.size() < (d.with_z_write ? v3d_shader::kFragmentZUniformWords : v3d_shader::kFragmentUniformWords))
    return false;
  uint32_t cs_values = 0, vs_values = 0;
  for (uint32_t i = 0; i < d.attribute_count; ++i) {
    const V3dAttribute& a = d.attributes[i];
    if (a.components < 1 || a.components > 4 || a.coordinate_shader_values > a.components || a.vertex_shader_values > a.components) return false;
    cs_values += a.coordinate_shader_values;
    vs_values += a.vertex_shader_values;
  }
  if (cs_values == 0 || vs_values == 0) return false;  // at least one attribute must be read by both shaders

  const uint32_t fs_uniforms = emit_uniforms(indirect, d.fragment_uniforms);
  const uint32_t vs_uniforms = emit_uniforms(indirect, d.vertex_uniforms);
  const uint32_t cs_uniforms = emit_uniforms(indirect, d.coordinate_uniforms);
  indirect.align(32);
  const uint32_t record_address = indirect.gpu_end();
  ArrayView<uint8_t> record = indirect.append(gl_shader_state_record::kBytes);
  if (!indirect.ok()) return false;

  const v3d_shader::Program& fs = d.with_z_write ? d.code.fragment_z : d.code.fragment;
  {
    V3dPacketBuilder p(record, true);
    namespace r = gl_shader_state_record;
    p.set(r::kEnableClipping, true);
    p.set(r::kFragmentShaderDoesZWrites, d.with_z_write).set(r::kTurnOffEarlyZTest, d.with_z_write);
    p.set(r::kDisableImplicitPointLineVaryings, true);
    p.set(r::kNumberOfVaryingsInFragmentShader, v3d_shader::kFragmentVaryings);
    p.set_address(r::kCoordinateShaderCodeAddress, d.code_address + d.code.coordinate.offset_bytes());
    p.set_address(r::kVertexShaderCodeAddress, d.code_address + d.code.vertex.offset_bytes());
    p.set_address(r::kFragmentShaderCodeAddress, d.code_address + fs.offset_bytes());
    p.set(r::kCoordinateShaderInputVPMSegmentSize, kVpmSegmentSize).set(r::kVertexShaderInputVPMSegmentSize, kVpmSegmentSize);
    p.set(r::kCoordinateShaderOutputVPMSegmentSize, kVpmSegmentSize).set(r::kVertexShaderOutputVPMSegmentSize, kVpmSegmentSize);
    p.set_address(r::kCoordinateShaderUniformsAddress, cs_uniforms).set_address(r::kVertexShaderUniformsAddress, vs_uniforms).set_address(r::kFragmentShaderUniformsAddress, fs_uniforms);
    p.set(r::kMinCoordShaderInputSegmentsRequiredInPlay, 1).set(r::kMinVertexShaderInputSegmentsRequiredInPlay, 1);
    p.set(r::kMinCoordShaderOutputSegmentsRequiredInPlayInAdditionToVCMCacheSize, 0).set(r::kMinVertexShaderOutputSegmentsRequiredInPlayInAdditionToVCMCacheSize, 0);
    p.set(r::kCoordinateShaderStartInFinalThreadSection, true).set(r::kVertexShaderStartInFinalThreadSection, true).set(r::kFragmentShaderStartInFinalThreadSection, true);
    p.set(r::kCoordinateShaderPropagateNaNs, true).set(r::kVertexShaderPropagateNaNs, true).set(r::kFragmentShaderPropagateNaNs, true);
    p.set_address(r::kAddressOfDefaultAttributeValues, d.defaults_address);
    if (!p.ok()) { indirect.mark_bad(); return false; }
  }
  for (uint32_t i = 0; i < d.attribute_count; ++i) {
    const V3dAttribute& a = d.attributes[i];
    ArrayView<uint8_t> bytes = indirect.append(gl_shader_state_attribute_record::kBytes);
    if (!indirect.ok()) return false;
    V3dPacketBuilder p(bytes, true);
    namespace r = gl_shader_state_attribute_record;
    p.set_address(r::kAddress, a.address);
    p.set(r::kVecSize, a.components & 3);  // 0 means 4
    p.set(r::kType, kAttributeFloat);
    p.set(r::kNumberOfValuesReadByCoordinateShader, a.coordinate_shader_values).set(r::kNumberOfValuesReadByVertexShader, a.vertex_shader_values);
    p.set(r::kStride, a.stride);
    p.set(r::kMaximumIndex, 0xFFFFFF);
    if (!p.ok()) { indirect.mark_bad(); return false; }
  }

  {
    V3dPacketBuilder p = bcl.emit(vcm_cache_size::kInfo);  // 4 batches of 16 vertices each way: the shaders' VPM rows are one sector, so the limit is the maximum
    p.set(vcm_cache_size::kNumberOf16VertexBatchesForBinning, 4).set(vcm_cache_size::kNumberOf16VertexBatchesForRendering, 4);
    if (!p.ok()) bcl.mark_bad();
  }
  {
    V3dPacketBuilder p = bcl.emit(gl_shader_state::kInfo);
    p.set_address(gl_shader_state::kAddress, record_address).set(gl_shader_state::kNumberOfAttributeArrays, d.attribute_count);
    if (!p.ok()) bcl.mark_bad();
  }
  if (d.indexed && d.base_vertex != 0) {  // for the next draw only
    V3dPacketBuilder p = bcl.emit(base_vertex_base_instance::kInfo);
    p.set(base_vertex_base_instance::kBaseVertex, static_cast<uint32_t>(d.base_vertex)).set(base_vertex_base_instance::kBaseInstance, 0);
    if (!p.ok()) bcl.mark_bad();
  }
  if (d.indexed) {
    {
      V3dPacketBuilder p = bcl.emit(index_buffer_setup::kInfo);
      p.set_address(index_buffer_setup::kAddress, d.index_address).set(index_buffer_setup::kSize, d.index_buffer_bytes);
      if (!p.ok()) bcl.mark_bad();
    }
    V3dPacketBuilder p = bcl.emit(indexed_prim_list::kInfo);
    p.set(indexed_prim_list::kMode, kPrimitiveTriangles);
    p.set(indexed_prim_list::kIndexType, d.index_bytes == 1 ? 0 : d.index_bytes == 2 ? 1 : 2);
    p.set(indexed_prim_list::kLength, d.vertex_count).set(indexed_prim_list::kIndexOffset, d.index_offset_bytes);
    if (!p.ok()) bcl.mark_bad();
  } else {
    V3dPacketBuilder p = bcl.emit(vertex_array_prims::kInfo);
    p.set(vertex_array_prims::kMode, kPrimitiveTriangles).set(vertex_array_prims::kLength, d.vertex_count).set(vertex_array_prims::kIndexOfFirstVertex, d.first_vertex);
    if (!p.ok()) bcl.mark_bad();
  }
  return bcl.ok() && indirect.ok();
}

}  // namespace hardware
