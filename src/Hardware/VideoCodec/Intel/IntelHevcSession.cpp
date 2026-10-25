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

#include "Hardware/VideoCodec/Intel/IntelHevcSession.hpp"

namespace hardware {

namespace {
constexpr uint64_t kBitstreamPadding = 64;   // zeros behind the last slice, for what the engine reads ahead
constexpr uint32_t kMaxSlices = 4096;
constexpr uint32_t kSliceDwords = 9 + 2 * 18 + 2 * 34 + 3;   // the most one slice segment's commands take
constexpr uint32_t kBatchTail = 4;                           // the end of the batch (a no-op and the end marker) and some room

// Where byte `target` of a NAL unit without its emulation prevention bytes is in the NAL unit as stored.
bool stored_offset(ConstByteView nal, uint64_t target, uint32_t* out) {
  uint64_t unescaped = 0;
  uint32_t zeros = 0;
  for (uint64_t i = 0; i < nal.size(); ++i) {
    const uint8_t b = nal[i];
    if (zeros >= 2 && b == 3) {
      zeros = 0;
      continue;
    }
    if (unescaped == target) {
      if (i > UINT32_MAX) return false;
      *out = static_cast<uint32_t>(i);
      return true;
    }
    ++unescaped;
    zeros = b == 0 ? zeros + 1 : 0;
  }
  return false;
}

// The engine's scaling lists from the stream's (both in raster order): flat 16 when the stream has none.
void scaling_for(const h265::Sps& sps, const h265::Pps& pps, intel_mfx::HevcScaling* out) {
  if (!sps.scaling_list_enabled) {
    for (auto& l : out->list4x4) ArrayView<uint8_t>(l, 16).fill(16);
    for (auto& l : out->list8x8) ArrayView<uint8_t>(l, 64).fill(16);
    for (auto& l : out->list16x16) ArrayView<uint8_t>(l, 64).fill(16);
    for (auto& l : out->list32x32) ArrayView<uint8_t>(l, 64).fill(16);
    ArrayView<uint8_t>(out->dc16x16, 6).fill(16);
    ArrayView<uint8_t>(out->dc32x32, 2).fill(16);
    return;
  }
  const h265::ScalingList& s = pps.scaling_list_present ? pps.scaling : sps.scaling;
  for (uint32_t i = 0; i < 6; ++i) {
    ArrayView<uint8_t>(out->list4x4[i], 16).copy_from(ConstByteView(s.list[0][i], 16));
    ArrayView<uint8_t>(out->list8x8[i], 64).copy_from(ConstByteView(s.list[1][i], 64));
    ArrayView<uint8_t>(out->list16x16[i], 64).copy_from(ConstByteView(s.list[2][i], 64));
    out->dc16x16[i] = s.dc[2][i];
  }
  for (uint32_t i = 0; i < 2; ++i) {   // (intra luma is matrix 0 of the 32x32 lists in the stream's numbering, inter luma matrix 3)
    ArrayView<uint8_t>(out->list32x32[i], 64).copy_from(ConstByteView(s.list[3][i * 3], 64));
    out->dc32x32[i] = s.dc[3][i * 3];
  }
}

// The widths (or heights) of the tile columns (or rows), in coding tree blocks.
void tile_sizes(bool uniform, uint32_t count, uint32_t total, const int* minus1, uint32_t* out) {
  uint32_t used = 0;
  for (uint32_t i = 0; i < count; ++i) {
    if (uniform) out[i] = static_cast<uint32_t>((uint64_t{i + 1} * total) / count - (uint64_t{i} * total) / count);
    else if (i + 1 < count) out[i] = static_cast<uint32_t>(minus1[i] + 1);
    else out[i] = used < total ? total - used : 0;
    used += out[i];
  }
}
}  // namespace

IntelHevcDecoder::IntelHevcDecoder(allocators::IAllocator& allocator, intel_gt::VcsDriver& driver, const DecoderConfig& config)
    : allocator_(allocator), config_(config), buffers_(allocator, driver), cpu_(allocator), host_bits_(allocator), jobs_(allocator), out_(allocator) {}

IntelHevcDecoder::~IntelHevcDecoder() = default;

bool IntelHevcDecoder::prepare() {
  if (!buffers_.lease()) return false;
  cpu_.set_offload(this);
  if (!config_.codec_config.empty() && !cpu_.configure(config_.codec_config)) {
    cpu_.set_offload(nullptr);
    buffers_.release_lease();
    return false;
  }
  return true;
}

bool IntelHevcDecoder::accepts(const h265::Sps& sps, const h265::Pps& pps) {
  if (!buffers_.leased()) return false;
  if (!buffers_.engine_ready()) return buffers_.is_open();   // a wedged engine still owns the stream it was decoding; a stream that never started can go to the CPU
  if (sps.chroma_format_idc != 1 || sps.bit_depth_luma != 8 || sps.bit_depth_chroma != 8 || sps.width < 16 || sps.height < 16) return false;
  if (static_cast<uint32_t>(sps.width) > IntelEngineBuffers::kMaxMbsPerSide * 16 || static_cast<uint32_t>(sps.height) > IntelEngineBuffers::kMaxMbsPerSide * 16) return false;
  if (sps.log2_ctb < 4 || sps.log2_ctb > 6) return false;
  return pps.num_tile_cols >= 1 && pps.num_tile_cols <= 19 && pps.num_tile_rows >= 1 && pps.num_tile_rows <= 21;
}

bool IntelHevcDecoder::build_frame_store(const h265::OffloadPicture& picture, FrameStore* store) const {
  for (int8_t& e : store->of) e = -1;
  store->count = 0;
  auto add = [&](const uint8_t* set, uint32_t n) {
    for (uint32_t i = 0; i < n && i < h265::kMaxRefs; ++i) {
      const uint8_t r = set[i];
      if (r >= picture.ref_count) return false;
      if (store->of[r] >= 0) continue;
      if (store->count == intel_mfx::kMaxHevcReferenceFrames) return false;
      store->of[r] = static_cast<int8_t>(store->count);
      store->ref[store->count++] = r;
    }
    return true;
  };
  return add(picture.st_before, picture.st_before_count) && add(picture.st_after, picture.st_after_count) && add(picture.lt_curr, picture.lt_count);
}

// The slice segments as the engine takes them: where each one starts in the picture and where the next one does, the slice data in the bitstream buffer (the NAL units one
// after the other, as stored) and where in each the slice segment data begins.
bool IntelHevcDecoder::build_slices(const h265::OffloadPicture& picture, const FrameStore& store) {
  using namespace intel_mfx;
  const h265::Sps& sps = *picture.sps;
  const h265::Pps& pps = *picture.pps;
  const uint64_t n = picture.slices.size();
  if (n == 0 || n > kMaxSlices || picture.slice_info.size() != n) return false;
  const uint32_t width_ctbs = static_cast<uint32_t>(sps.pic_w_ctb), total_ctbs = width_ctbs * static_cast<uint32_t>(sps.pic_h_ctb);
  if (width_ctbs == 0 || total_ctbs == 0) return false;
  jobs_.clear();
  host_bits_.clear();
  // The reference list entry `i` of list `l` of slice `k`: the frame store it is in (-1: none) and the reference's index.
  auto entry = [&](uint64_t k, int l, int i, int* ref) {
    const uint8_t r = picture.slice_info[k].list[l][i];
    if (r >= picture.ref_count || store.of[r] < 0) return -1;
    *ref = r;
    return static_cast<int>(store.of[r]);
  };
  bool first_inter_valid = false, first_from_l0 = false;
  uint32_t first_collocated = 0;
  for (uint64_t k = 0; k < n; ++k) {
    const ConstByteView nal = picture.slices[k];
    const h265::SliceHeader* header = picture.slice_info[k].header;
    if (!header || nal.size() == 0 || nal.size() > UINT32_MAX / 2 || host_bits_.size() > UINT32_MAX / 2) return false;
    const h265::SliceHeader& sh = *header;
    if (sh.num_ref_idx[0] < 0 || sh.num_ref_idx[1] < 0 || sh.num_ref_idx[0] > static_cast<int>(kMaxHevcRefIdx) || sh.num_ref_idx[1] > static_cast<int>(kMaxHevcRefIdx) ||
        sh.segment_address < 0 || static_cast<uint32_t>(sh.segment_address) >= total_ctbs || sh.data_bit_offset % 8 != 0)
      return false;
    HevcSlice j;
    j.hor_pos = static_cast<uint32_t>(sh.segment_address) % width_ctbs;
    j.ver_pos = static_cast<uint32_t>(sh.segment_address) / width_ctbs;
    j.slice_type = static_cast<uint32_t>(sh.type);
    j.qp = sh.slice_qp;
    j.cb_qp_offset = sh.cb_qp_offset;
    j.cr_qp_offset = sh.cr_qp_offset;
    j.temporal_mvp = sh.temporal_mvp;
    j.dependent = sh.dependent_slice_segment;
    j.cabac_init = sh.cabac_init_flag;
    j.mvd_l1_zero = sh.mvd_l1_zero;
    j.collocated_from_l0 = sh.collocated_from_l0;
    j.sao_luma = sh.sao_luma;
    j.sao_chroma = sh.sao_chroma;
    j.loop_filter_across_slices = sh.loop_filter_across_slices;
    j.deblocking_disabled = sh.deblocking_disabled;
    j.beta_offset_div2 = sh.beta_offset_div2;
    j.tc_offset_div2 = sh.tc_offset_div2;
    j.max_merge_candidates = sh.type == h265::kSliceI ? 5 : static_cast<uint32_t>(sh.max_num_merge_cand);
    if (j.max_merge_candidates < 1 || j.max_merge_candidates > 5) return false;
    const bool weighted = (sh.type == h265::kSliceP && pps.weighted_pred) || (sh.type == h265::kSliceB && pps.weighted_bipred);
    if (weighted) {
      j.luma_log2_weight_denom = static_cast<uint32_t>(sh.luma_log2_denom);
      j.chroma_log2_weight_denom = static_cast<uint32_t>(sh.chroma_log2_denom);
    }
    // The pictures the slice predicts from: all must be in the frame store; low delay when none follows the picture in output order.
    bool low_delay = sh.type != h265::kSliceI;
    const int lists = sh.type == h265::kSliceB ? 2 : sh.type == h265::kSliceP ? 1 : 0;
    for (int l = 0; l < lists; ++l)
      for (int i = 0; i < sh.num_ref_idx[l]; ++i) {
        int ref = 0;
        if (entry(k, l, i, &ref) < 0) return false;
        if (picture.refs[ref].poc > picture.poc) low_delay = false;
      }
    j.low_delay = low_delay;
    // The collocated picture, as a frame store.
    uint32_t collocated = 0;
    if (sh.temporal_mvp && sh.type != h265::kSliceI && sh.collocated_ref_idx >= 0 && sh.collocated_ref_idx <= 14) {
      const int l = sh.type == h265::kSliceP || sh.collocated_from_l0 ? 0 : 1;
      int ref = 0;
      const int fs = sh.collocated_ref_idx < sh.num_ref_idx[l] ? entry(k, l, sh.collocated_ref_idx, &ref) : -1;
      if (fs >= 0) collocated = static_cast<uint32_t>(fs);
    }
    // The engine wants the collocated picture of the first inter slice with the temporal predictor on to stay the same for the slices after it that have none.
    bool from_l0 = j.collocated_from_l0;
    if (!first_inter_valid && sh.type != h265::kSliceI && sh.temporal_mvp) {
      first_inter_valid = true;
      first_collocated = collocated;
      first_from_l0 = from_l0;
    }
    if (first_inter_valid && (sh.type == h265::kSliceI || !sh.temporal_mvp)) {
      collocated = first_collocated;
      from_l0 = first_from_l0;
    }
    j.collocated_frame_store = collocated;
    j.collocated_from_l0 = from_l0;
    uint32_t header_bytes = 0;
    if (!stored_offset(nal, 2 + sh.data_bit_offset / 8, &header_bytes)) return false;
    j.header_bytes = header_bytes;
    j.data_offset = static_cast<uint32_t>(host_bits_.size());
    j.data_size = static_cast<uint32_t>(nal.size());
    if (!host_bits_.append(nal) || !jobs_.push_back(j)) return false;
  }
  for (uint64_t k = 0; k < n; ++k) {
    HevcSlice& j = jobs_[k];
    j.last = k + 1 == n;
    if (!j.last) {
      const int next = picture.slice_info[k + 1].header ? picture.slice_info[k + 1].header->segment_address : -1;
      if (next < 0 || static_cast<uint32_t>(next) >= total_ctbs) return false;
      j.next_hor_pos = static_cast<uint32_t>(next) % width_ctbs;
      j.next_ver_pos = static_cast<uint32_t>(next) / width_ctbs;
    }
  }
  return true;
}

bool IntelHevcDecoder::fail(const h265::OffloadPicture& picture, bool grey) {
  ++failed_;
  if (grey) buffers_.grey(picture.slot);
  return false;
}

bool IntelHevcDecoder::decode_picture(const h265::OffloadPicture& picture) {
  using namespace intel_mfx;
  if (!picture.sps || !picture.pps || !buffers_.engine_ready()) return fail(picture, false);
  const h265::Sps& sps = *picture.sps;
  const h265::Pps& pps = *picture.pps;
  const uint32_t width = static_cast<uint32_t>(sps.width), height = static_cast<uint32_t>(sps.height);
  if (!buffers_.is_open() || buffers_.width() != width || buffers_.height() != height) {
    if (!buffers_.open(width, height)) return fail(picture, false);
  }
  FrameStore store;
  if (!build_frame_store(picture, &store)) return fail(picture, false);

  // The buffers: this picture, the pictures in the frame store with the motion vectors they left, and the engine's line and column buffers.
  HevcBuffers b;
  const HevcBufferBytes sizes = hevc_buffer_bytes(width, height, static_cast<uint32_t>(sps.log2_ctb));
  uint32_t address = 0;
  auto place = [&](uint32_t index, uint64_t bytes, uint64_t* out) {
    if (!buffers_.scratch(index, bytes ? bytes : 1, &address)) return false;
    *out = address;
    return true;
  };
  if (!buffers_.surface(picture.slot, &address)) return fail(picture, false);
  b.current_picture = address;
  if (!buffers_.picture_buffer(picture.slot, sizes.motion_vector ? sizes.motion_vector : 1, &address)) return fail(picture, false);
  b.motion_vector_current = address;
  for (uint32_t f = 0; f < store.count; ++f) {
    const int slot = picture.refs[store.ref[f]].slot;
    if (!buffers_.surface(slot, &address)) return fail(picture, false);
    b.reference_pictures[f] = address;
    if (!buffers_.picture_buffer(slot, sizes.motion_vector ? sizes.motion_vector : 1, &address)) return fail(picture, false);
    b.motion_vector_references[f] = address;
  }
  if (!place(0, sizes.deblocking_line, &b.deblocking_line) || !place(1, sizes.deblocking_tile_line, &b.deblocking_tile_line) ||
      !place(2, sizes.deblocking_tile_column, &b.deblocking_tile_column) || !place(3, sizes.metadata_line, &b.metadata_line) ||
      !place(4, sizes.metadata_tile_line, &b.metadata_tile_line) || !place(5, sizes.metadata_tile_column, &b.metadata_tile_column) || !place(6, sizes.sao_line, &b.sao_line) ||
      !place(7, sizes.sao_tile_line, &b.sao_tile_line) || !place(8, sizes.sao_tile_column, &b.sao_tile_column))
    return fail(picture, false);

  if (!build_slices(picture, store)) return fail(picture, true);
  if (!host_bits_.put_zeros(kBitstreamPadding) || !buffers_.upload_bitstream(host_bits_.view(), &address)) return fail(picture, false);
  b.bitstream = address;
  b.bitstream_bytes = host_bits_.size();

  HevcPicture p;
  p.width = width;
  p.height = height;
  p.log2_min_cb = static_cast<uint32_t>(sps.log2_min_cb);
  p.log2_ctb = static_cast<uint32_t>(sps.log2_ctb);
  p.log2_min_tb = static_cast<uint32_t>(sps.log2_min_tb);
  p.log2_max_tb = static_cast<uint32_t>(sps.log2_max_tb);
  p.pcm = sps.pcm;
  p.pcm_loop_filter_disabled = sps.pcm_loop_filter_disabled;
  p.log2_min_pcm = static_cast<uint32_t>(sps.log2_min_pcm);
  p.log2_max_pcm = static_cast<uint32_t>(sps.log2_max_pcm);
  p.pcm_bit_depth_luma = static_cast<uint32_t>(sps.pcm_bit_depth_luma);
  p.pcm_bit_depth_chroma = static_cast<uint32_t>(sps.pcm_bit_depth_chroma);
  p.strong_intra_smoothing = sps.strong_intra_smoothing;
  p.transquant_bypass = pps.transquant_bypass;
  p.amp = sps.amp;
  p.transform_skip = pps.transform_skip;
  p.weighted_pred = pps.weighted_pred;
  p.weighted_bipred = pps.weighted_bipred;
  p.sign_data_hiding = pps.sign_data_hiding;
  p.constrained_intra_pred = pps.constrained_intra_pred;
  p.cu_qp_delta = pps.cu_qp_delta_enabled;
  p.sao = sps.sao;
  p.tiles = pps.tiles_enabled;
  p.entropy_coding_sync = pps.entropy_coding_sync;
  p.loop_filter_across_tiles = pps.loop_filter_across_tiles;
  p.log2_parallel_merge_level = static_cast<uint32_t>(pps.log2_parallel_merge_level);
  p.diff_cu_qp_delta_depth = static_cast<uint32_t>(pps.diff_cu_qp_delta_depth);
  p.cb_qp_offset = pps.cb_qp_offset;
  p.cr_qp_offset = pps.cr_qp_offset;
  p.max_transform_hierarchy_depth_inter = static_cast<uint32_t>(sps.max_th_depth_inter);
  p.max_transform_hierarchy_depth_intra = static_cast<uint32_t>(sps.max_th_depth_intra);
  if (pps.tiles_enabled) {
    p.num_tile_columns = static_cast<uint32_t>(pps.num_tile_cols);
    p.num_tile_rows = static_cast<uint32_t>(pps.num_tile_rows);
    tile_sizes(pps.uniform_spacing, p.num_tile_columns, static_cast<uint32_t>(sps.pic_w_ctb), pps.col_width_minus1, p.column_width);
    tile_sizes(pps.uniform_spacing, p.num_tile_rows, static_cast<uint32_t>(sps.pic_h_ctb), pps.row_height_minus1, p.row_height);
  }
  HevcScaling scaling;
  scaling_for(sps, pps, &scaling);

  // The batches: everything but the slices, then as many slices as fit; a picture with more slices than a batch holds goes in several.
  const uint64_t capacity = buffers_.batch_words().size() < batch_limit_ ? buffers_.batch_words().size() : batch_limit_;
  uint64_t next = 0;
  while (next < jobs_.count()) {
    DwordWriter w(buffers_.batch_words());
    emit_mi_flush(w);
    emit_hcp_pipe_mode_select(w, kHcpCodecHevc);
    emit_hcp_surface_state(w, buffers_.layout());
    emit_hevc_pipe_buf_addr_state(w, b);
    emit_hevc_qm_states(w, scaling);
    emit_hevc_pic_state(w, p);
    if (pps.tiles_enabled) emit_hevc_tile_state(w, p);
    emit_hevc_ind_obj_base_addr_state(w, b);
    const uint64_t first = next;
    while (next < jobs_.count() && (next == first || w.count() + kSliceDwords + kBatchTail <= capacity)) {
      const HevcSlice& j = jobs_[next];
      const h265::OffloadSlice& info = picture.slice_info[next];
      const h265::SliceHeader& sh = *info.header;
      emit_hevc_slice_state(w, j);
      const int lists = sh.type == h265::kSliceB ? 2 : sh.type == h265::kSliceP ? 1 : 0;
      for (int l = 0; l < lists; ++l) {
        uint32_t entries[kMaxHevcRefIdx] = {};
        for (int i = 0; i < sh.num_ref_idx[l] && i < static_cast<int>(kMaxHevcRefIdx); ++i) {
          const h265::OffloadReference& ref = picture.refs[info.list[l][i]];
          entries[i] = hevc_ref_idx_entry(static_cast<uint32_t>(store.of[info.list[l][i]]), ref.is_long, picture.poc - ref.poc);
        }
        emit_hevc_ref_idx_state(w, static_cast<uint32_t>(l), static_cast<uint32_t>(sh.num_ref_idx[l]), entries);
      }
      if ((sh.type == h265::kSliceP && pps.weighted_pred) || (sh.type == h265::kSliceB && pps.weighted_bipred)) {
        for (int l = 0; l < lists; ++l) {
          HevcWeights t;
          for (int i = 0; i < sh.num_ref_idx[l] && i < static_cast<int>(kMaxHevcRefIdx); ++i) {
            t.luma_weight[i] = static_cast<int8_t>(sh.luma_weight[l][i] - (1 << sh.luma_log2_denom));
            t.luma_offset[i] = static_cast<int8_t>(sh.luma_offset[l][i]);
            for (int c = 0; c < 2; ++c) {
              t.chroma_weight[i][c] = static_cast<int8_t>(sh.chroma_weight[l][i][c] - (1 << sh.chroma_log2_denom));
              t.chroma_offset[i][c] = static_cast<int8_t>(sh.chroma_offset[l][i][c]);
            }
          }
          emit_hevc_weightoffset_state(w, static_cast<uint32_t>(l), static_cast<uint32_t>(sh.num_ref_idx[l]), t);
        }
      }
      emit_hevc_bsd_object(w, j);
      ++next;
    }
    emit_batch_end(w);
    if (!w.ok() || !buffers_.run(ArrayView<const uint32_t>(buffers_.batch_words().data(), w.count()))) return fail(picture, true);
  }
  if (!buffers_.read_back(picture.slot, picture.plane[0], static_cast<uint32_t>(picture.stride[0]), picture.plane[1], static_cast<uint32_t>(picture.stride[1]), picture.plane[2],
                          static_cast<uint32_t>(picture.stride[2]), 0, 1))
    return fail(picture, false);
  ++decoded_;
  return true;
}

SubmitResult IntelHevcDecoder::submit(const VideoPacket& packet) {
  if (!buffers_.leased()) {
    ++stats_.errors;
    return SubmitResult::Invalid;
  }
  if (cpu_.ready()) return SubmitResult::Full;
  if (!cpu_.decode(packet.data, packet.pts_us)) {
    ++stats_.errors;
    return SubmitResult::Invalid;
  }
  ++stats_.packets;
  stats_.bytes += packet.data.size();
  return SubmitResult::Accepted;
}

bool IntelHevcDecoder::receive(VideoFrame* frame) {
  VideoFrame in;
  if (!cpu_.receive(&in)) return false;
  if (!out_.allocate({in.format.width, in.format.height, config_.output}) || !convert_frame(in, out_)) {
    ++stats_.errors;
    return false;
  }
  *frame = out_.frame(in.pts_us, in.keyframe);
  frame->limited_range = in.limited_range && config_.output != PixelFormat::Bgra8888;
  ++stats_.frames;
  return true;
}

VideoFormat IntelHevcDecoder::output_format() const {
  VideoFormat f = cpu_.output_format();
  f.pixel_format = config_.output;
  return f;
}

VideoCodecStatistics IntelHevcDecoder::statistics() const {
  VideoCodecStatistics s = stats_;
  s.errors += failed_;
  return s;
}

}  // namespace hardware
