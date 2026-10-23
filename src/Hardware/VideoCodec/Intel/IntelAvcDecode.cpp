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

#include "Hardware/VideoCodec/Intel/IntelAvcDecode.hpp"

namespace hardware {
namespace intel_mfx {

namespace {
constexpr uint32_t kMaxMbsPerSide = 256;  // 4096 pixels

bool slice_has_refs(const h264::SliceHeader& h) { return h.type != h264::kSliceI; }

uint32_t align8(uint32_t v) { return (v + 7) & ~7u; }
}  // namespace

bool make_avc_picture(const h264::Sps& sps, const h264::Pps& pps, const h264::SliceHeader& first, AvcPicture* out) {
  if (sps.mb_w < 1 || sps.mb_h < 1 || static_cast<uint32_t>(sps.mb_w) > kMaxMbsPerSide || static_cast<uint32_t>(sps.mb_h) > kMaxMbsPerSide) return false;
  if (sps.chroma_format_idc != 1 || sps.mbaff || first.field_pic) return false;
  AvcPicture p;
  p.width_in_mbs = static_cast<uint32_t>(sps.mb_w);
  p.height_in_mbs = static_cast<uint32_t>(sps.mb_h);
  p.chroma_qp_index_offset = pps.chroma_qp_offset[0];
  p.second_chroma_qp_index_offset = pps.chroma_qp_offset[1];
  p.weighted_pred = pps.weighted_pred;
  p.weighted_bipred_idc = static_cast<uint32_t>(pps.weighted_bipred_idc);
  p.chroma_format_idc = static_cast<uint32_t>(sps.chroma_format_idc);
  p.entropy_coding_cabac = pps.cabac;
  p.reference_picture = first.nal_ref_idc != 0;
  p.constrained_intra_pred = pps.constrained_intra_pred;
  p.direct_8x8_inference = sps.direct_8x8_inference;
  p.transform_8x8_mode = pps.transform_8x8;
  p.frame_mbs_only = sps.frame_mbs_only;
  *out = p;
  return true;
}

bool make_avc_slice(const AvcPicture& picture, const h264::Pps& pps, const H264Slice& slice, const H264Slice* next, AvcSlice* out) {
  const h264::SliceHeader& h = slice.header;
  const uint32_t total_mbs = picture.width_in_mbs * picture.height_in_mbs;
  if (static_cast<uint32_t>(h.first_mb) >= total_mbs) return false;
  if (next && (static_cast<uint32_t>(next->header.first_mb) >= total_mbs || next->header.first_mb <= h.first_mb)) return false;
  if (slice.nal_size == 0 || slice.header_bits > slice.nal_size * 8) return false;
  AvcSlice s;
  s.slice_type = static_cast<uint32_t>(h.type);
  s.first_mb = static_cast<uint32_t>(h.first_mb);
  s.last_slice = next == nullptr;
  s.next_first_mb = next ? static_cast<uint32_t>(next->header.first_mb) : 0;
  s.num_ref_idx_l0 = slice_has_refs(h) ? static_cast<uint32_t>(h.num_ref_idx[0]) : 0;
  s.num_ref_idx_l1 = h.type == h264::kSliceB ? static_cast<uint32_t>(h.num_ref_idx[1]) : 0;
  s.luma_log2_weight_denom = static_cast<uint32_t>(h.luma_log2_denom);
  s.chroma_log2_weight_denom = static_cast<uint32_t>(h.chroma_log2_denom);
  s.direct_spatial_mv_pred = h.direct_spatial_mv_pred;
  s.disable_deblocking_filter_idc = static_cast<uint32_t>(h.disable_deblocking);
  s.cabac_init_idc = static_cast<uint32_t>(h.cabac_init_idc);
  s.slice_qp = h.slice_qp;
  s.slice_alpha_c0_offset_div2 = h.alpha_off / 2;
  s.slice_beta_offset_div2 = h.beta_off / 2;
  s.data_size = static_cast<uint32_t>(slice.nal_size);
  s.data_offset = static_cast<uint32_t>(slice.nal_offset);
  // The engine undoes the emulation prevention itself: the offset counts the NAL header and the payload bits without those bytes. CABAC data starts
  // on a byte boundary (cabac_alignment_one_bit).
  uint32_t bits = static_cast<uint32_t>(slice.header_bits);
  if (pps.cabac) bits = align8(bits);
  s.first_mb_bit_offset = bits;
  *out = s;
  return true;
}

AvcBatchError build_avc_decode_batch(const AvcPictureJob& job, ArrayView<uint32_t> out, uint64_t* dwords) {
  *dwords = 0;
  if (!job.sets || !job.sps || !job.pps || job.slices.empty() || job.bitstream_address == 0 || job.buffers.current_picture == 0) return AvcBatchError::BadInput;
  const uint64_t au_size = job.access_unit.size();
  for (const H264Slice& s : job.slices) {
    if (s.nal_offset > au_size || s.nal_size > au_size - s.nal_offset || s.nal_size > UINT32_MAX || s.nal_offset > UINT32_MAX) return AvcBatchError::BadInput;
  }
  AvcPicture picture;
  if (!make_avc_picture(*job.sps, *job.pps, job.slices[0].header, &picture)) return AvcBatchError::Unsupported;
  Nv12TiledLayout layout;
  if (!nv12_tiled_layout(picture.width_in_mbs * 16, picture.height_in_mbs * 16, &layout)) return AvcBatchError::BadInput;

  AvcBuffers buffers = job.buffers;
  uint32_t used_frames = 0;
  for (uint32_t i = 0; i < kMaxAvcReferenceFrames; ++i) {
    const AvcFrameStore& f = job.frames[i];
    if (!f.used) continue;
    if (f.picture == 0 || f.direct_mv == 0) return AvcBatchError::BadInput;  // a reference the engine cannot read would hang it
    ++used_frames;
    buffers.reference_pictures[i] = f.picture;
    buffers.direct_mv_references[i] = f.direct_mv;
    buffers.reference_poc[i][0] = f.poc[0];
    buffers.reference_poc[i][1] = f.poc[1];
  }
  buffers.current_poc[0] = job.current_poc[0];
  buffers.current_poc[1] = job.current_poc[1];
  buffers.bitstream = job.bitstream_address;
  buffers.deblocking_enabled = false;
  for (const H264Slice& s : job.slices)
    if (s.header.disable_deblocking != 1) buffers.deblocking_enabled = true;

  uint8_t w4[6][16], w8[2][64];
  job.sets->effective_scaling(*job.sps, *job.pps, w4, w8);

  DwordWriter w(out);
  emit_mi_flush(w);
  emit_pipe_mode_select(w, buffers.deblocking_enabled, !buffers.deblocking_enabled);
  emit_surface_state(w, picture.width_in_mbs * 16, picture.height_in_mbs * 16, layout);
  emit_pipe_buf_addr_state(w, buffers);
  emit_bsp_buf_base_addr_state(w, buffers);
  uint8_t matrix[48];
  for (uint32_t list = 0; list < 2; ++list) {
    for (uint32_t k = 0; k < 3; ++k)
      for (uint32_t i = 0; i < 16; ++i) matrix[k * 16 + i] = w4[list * 3 + k][i];
    emit_qm_state(w, list == 0 ? kQmAvc4x4Intra : kQmAvc4x4Inter, ConstByteView(matrix, sizeof matrix));
  }
  if (picture.transform_8x8_mode) {
    emit_qm_state(w, kQmAvc8x8Intra, ConstByteView(w8[0], 64));
    emit_qm_state(w, kQmAvc8x8Inter, ConstByteView(w8[1], 64));
  }
  emit_avc_picid_state(w);
  emit_avc_img_state(w, picture);
  emit_ind_obj_base_addr_state(w, buffers.bitstream, buffers.mocs);

  const uint64_t count = job.slices.size();
  if (job.slices[0].header.first_mb != 0) {
    emit_avc_phantom_slice(w, picture, true, static_cast<uint32_t>(job.slices[0].header.first_mb));
  }
  for (uint64_t i = 0; i < count; ++i) {
    const H264Slice& slice = job.slices[i];
    const H264Slice* next = i + 1 < count ? &job.slices[i + 1] : nullptr;
    AvcSlice s;
    if (!make_avc_slice(picture, *job.pps, slice, next, &s)) return AvcBatchError::BadInput;
    const h264::SliceHeader& h = slice.header;
    emit_avc_directmode_state(w, buffers);
    if (slice_has_refs(h)) {
      if (used_frames == 0 || !job.ref_list0) return AvcBatchError::BadInput;
      for (uint32_t list = 0; list < (h.type == h264::kSliceB ? 2u : 1u); ++list) {
        const uint8_t(*lists)[kMaxAvcRefIdx] = list == 0 ? job.ref_list0 : job.ref_list1;
        if (!lists) return AvcBatchError::BadInput;
        uint8_t entries[kMaxAvcRefIdx];
        const uint32_t n = list == 0 ? s.num_ref_idx_l0 : s.num_ref_idx_l1;
        for (uint32_t k = 0; k < kMaxAvcRefIdx; ++k) {
          entries[k] = 0xFF;
          if (k >= n) continue;
          const uint8_t idx = lists[i][k];
          if (idx >= kMaxAvcReferenceFrames || !job.frames[idx].used) return AvcBatchError::BadInput;
          entries[k] = avc_ref_idx_entry(idx, job.frames[idx].long_term);
        }
        emit_avc_ref_idx_state(w, list, entries);
      }
    }
    uint32_t tables = 0;
    if (h.type == h264::kSliceP && picture.weighted_pred) tables = 1;
    if (h.type == h264::kSliceB && picture.weighted_bipred_idc == 1) tables = 2;
    for (uint32_t t = 0; t < tables; ++t) {
      int16_t table[kMaxAvcRefIdx * 6];
      for (uint32_t k = 0; k < kMaxAvcRefIdx; ++k) {
        table[k * 6 + 0] = static_cast<int16_t>(h.luma_weight[t][k]);
        table[k * 6 + 1] = static_cast<int16_t>(h.luma_offset[t][k]);
        table[k * 6 + 2] = static_cast<int16_t>(h.chroma_weight[t][k][0]);
        table[k * 6 + 3] = static_cast<int16_t>(h.chroma_offset[t][k][0]);
        table[k * 6 + 4] = static_cast<int16_t>(h.chroma_weight[t][k][1]);
        table[k * 6 + 5] = static_cast<int16_t>(h.chroma_offset[t][k][1]);
      }
      emit_avc_weightoffset_state(w, t, table);
    }
    emit_avc_slice_state(w, picture, s);
    emit_avc_bsd_object(w, s);
  }
  emit_batch_end(w);
  *dwords = w.count();
  return w.ok() ? AvcBatchError::None : AvcBatchError::NoRoom;
}

}  // namespace intel_mfx
}  // namespace hardware
