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

#include "Hardware/VideoCodec/Intel/IntelVp9Session.hpp"

namespace hardware {

namespace {
constexpr uint64_t kBitstreamPadding = 64;   // zeros behind the frame, for what the engine reads ahead
constexpr uint32_t kMinimumSide = 16;
constexpr uint32_t kProbabilityScratch = 8, kSegmentIdScratch = 9;

uint32_t align8(uint32_t v) { return (v + 7) & ~7u; }
}  // namespace

IntelVp9Decoder::IntelVp9Decoder(allocators::IAllocator& allocator, intel_gt::VcsDriver& driver, const DecoderConfig& config)
    : allocator_(allocator), config_(config), buffers_(allocator, driver), cpu_(allocator), host_bits_(allocator), host_planes_(allocator), out_(allocator) {
  forget_references();
}

IntelVp9Decoder::~IntelVp9Decoder() = default;

void IntelVp9Decoder::forget_references() {
  for (int& s : ref_surface_) s = -1;
  have_last_ = false;
  contexts_.reset();
}

bool IntelVp9Decoder::prepare() {
  if (!buffers_.lease()) return false;
  cpu_.set_offload(this);
  return true;
}

void IntelVp9Decoder::reset() {
  cpu_.reset();
  forget_references();
}

bool IntelVp9Decoder::accepts(const vp9::FrameHeaderInfo& info, int ss_x, int ss_y) {
  if (!buffers_.leased()) return false;
  if (!buffers_.engine_ready()) return buffers_.is_open();   // a wedged engine still owns the stream it was decoding; a stream that never started can go to the CPU
  if (info.profile != 0 || info.bit_depth != 8 || ss_x != 1 || ss_y != 1 || info.width <= 0 || info.height <= 0) return false;
  const uint32_t width = align8(static_cast<uint32_t>(info.width)), height = align8(static_cast<uint32_t>(info.height));
  return width >= kMinimumSide && height >= kMinimumSide && width <= IntelEngineBuffers::kMaxMbsPerSide * 16 && height <= IntelEngineBuffers::kMaxMbsPerSide * 16;
}

// A surface no reference slot holds.
int IntelVp9Decoder::pick_surface() const {
  for (int s = 0; s < static_cast<int>(IntelEngineBuffers::kMaxSurfaces); ++s) {
    bool used = false;
    for (int r : ref_surface_) used = used || r == s;
    if (!used) return s;
  }
  return -1;
}

bool IntelVp9Decoder::run_frame(const vp9::OffloadFrame& frame, int current) {
  using namespace intel_mfx;
  const uint32_t width = static_cast<uint32_t>(frame.info.width), height = static_cast<uint32_t>(frame.info.height);
  const uint32_t aligned_width = align8(width), aligned_height = align8(height);
  const bool key = frame.info.key_frame, intra_only = frame.info.intra_only;
  const bool intra = key || intra_only;
  if (!buffers_.engine_ready() || frame.data.size() <= frame.uncompressed_header_bytes || frame.data.size() > UINT32_MAX / 2) return false;
  // A key frame may start the stream anew, at another size.
  if (!buffers_.is_open() || (key && (buffers_.width() != aligned_width || buffers_.height() != aligned_height))) {
    forget_references();
    if (!buffers_.open(aligned_width, aligned_height)) return false;
  }
  if (buffers_.width() != aligned_width || buffers_.height() != aligned_height) return false;   // (a frame of another size would need scaled references)

  // The pictures the frame predicts from.
  Vp9Picture p;
  Vp9Buffers b;
  uint32_t address = 0;
  if (!buffers_.surface(current, &address)) return false;
  b.current_picture = address;
  if (!intra)
    for (int i = 0; i < 3; ++i) {
      const int slot = frame.ref_slot[i];
      if (slot < 0 || slot > 7 || ref_surface_[slot] < 0 || !buffers_.surface(ref_surface_[slot], &address)) return false;
      b.reference_pictures[i] = address;
      p.reference_present[i] = true;
      p.reference_width[i] = surface_width_[ref_surface_[slot]];
      p.reference_height[i] = surface_height_[ref_surface_[slot]];
    }

  // The engine's line buffers, the probabilities, the segment ids and the collocated motion vectors.
  const Vp9BufferBytes sizes = vp9_buffer_bytes(width, height);
  auto place = [&](uint32_t index, uint64_t bytes, uint64_t* out) {
    if (!buffers_.scratch(index, bytes, &address)) return false;
    *out = address;
    return true;
  };
  if (!place(0, sizes.deblocking_line, &b.deblocking_line) || !place(1, sizes.deblocking_tile_line, &b.deblocking_tile_line) ||
      !place(2, sizes.deblocking_tile_column, &b.deblocking_tile_column) || !place(3, sizes.metadata_line, &b.metadata_line) ||
      !place(4, sizes.metadata_tile_line, &b.metadata_tile_line) || !place(5, sizes.metadata_tile_column, &b.metadata_tile_column) || !place(6, sizes.hvd_line, &b.hvd_line) ||
      !place(7, sizes.hvd_tile, &b.hvd_tile) || !place(kProbabilityScratch, sizes.probability, &b.probability) || !place(kSegmentIdScratch, sizes.segment_id, &b.segment_id) ||
      !place(mv_current_, sizes.motion_vector, &b.motion_vector_current) || !place(mv_last_, sizes.motion_vector, &b.motion_vector_last))
    return false;
  const bool scaled = have_last_ && (last_width_ != width || last_height_ != height);
  if ((key || frame.error_resilient || intra_only || scaled) && !buffers_.clear_scratch(kSegmentIdScratch)) return false;   // (the segment map starts over)

  // The probabilities the frame starts from.
  intel_vp9::ProbabilityContexts::Frame pf;
  pf.key = key;
  pf.intra_only = intra_only;
  pf.error_resilient = frame.error_resilient;
  pf.refresh_frame_context = frame.refresh_frame_context;
  pf.reset_frame_context = static_cast<uint32_t>(frame.reset_frame_context);
  pf.frame_context_idx = static_cast<uint32_t>(frame.frame_context_idx);
  pf.segmentation_enabled = frame.seg.enabled;
  pf.segmentation_update_map = frame.seg.update_map;
  for (int i = 0; i < 7; ++i) pf.segment_tree_probs[i] = frame.seg.tree_probs[i];
  for (int i = 0; i < 3; ++i) pf.segment_pred_probs[i] = frame.seg.pred_probs[i];
  uint8_t previous[intel_vp9::kProbabilityBufferBytes], probabilities[intel_vp9::kProbabilityBufferBytes];
  if (!buffers_.read_scratch(kProbabilityScratch, ByteView(previous, sizeof previous))) return false;
  contexts_.prepare(pf, ConstByteView(previous, sizeof previous), ByteView(probabilities, sizeof probabilities));
  if (!buffers_.upload_scratch(kProbabilityScratch, ConstByteView(probabilities, sizeof probabilities))) return false;

  // The frame itself, as it is (the compressed header and the tiles are the engine's to read).
  host_bits_.clear();
  if (!host_bits_.append(frame.data) || !host_bits_.put_zeros(kBitstreamPadding) || !buffers_.upload_bitstream(host_bits_.view(), &address)) return false;
  b.bitstream = address;
  b.bitstream_bytes = host_bits_.size();

  p.width = aligned_width;
  p.height = aligned_height;
  p.frame_width = width;
  p.frame_height = height;
  p.segmentation_enabled = frame.seg.enabled;
  p.segment_id_out = frame.seg.enabled && frame.seg.update_map;
  p.segmentation_temporal_update = intra ? false : frame.seg.temporal_update;
  if (intra) p.segment_id_in = true;
  else if (frame.seg.enabled) p.segment_id_in = !frame.seg.update_map || frame.seg.temporal_update;
  if (frame.error_resilient) p.segment_id_in = true;
  p.lossless = frame.base_qindex == 0 && frame.y_dc_delta_q == 0 && frame.uv_dc_delta_q == 0 && frame.uv_ac_delta_q == 0;
  p.sharpness_level = static_cast<uint32_t>(frame.sharpness_level);
  p.filter_level = static_cast<uint32_t>(frame.filter_level);
  p.frame_parallel = frame.frame_parallel;
  p.error_resilient = frame.error_resilient;
  p.refresh_frame_context = frame.refresh_frame_context;
  p.last_frame_type = last_frame_type_;
  p.use_prev_frame_mvs = !(frame.error_resilient || !have_last_ || width != last_width_ || height != last_height_ || intra_only || key || last_intra_only_ ||
                           last_frame_type_ == 0 || !last_show_);
  p.last_ref_sign_bias = frame.sign_bias[0] != 0;
  p.golden_ref_sign_bias = frame.sign_bias[1] != 0;
  p.alt_ref_sign_bias = frame.sign_bias[2] != 0;
  p.interp_filter = static_cast<uint32_t>(frame.interp_filter);
  p.allow_high_precision_mv = frame.hp_mv;
  p.intra_only = intra_only;
  p.adapt_probabilities = !(frame.error_resilient || frame.frame_parallel);
  p.frame_type = key ? 0 : 1;
  p.log2_tile_columns = static_cast<uint32_t>(frame.log2_tile_cols);
  p.log2_tile_rows = static_cast<uint32_t>(frame.log2_tile_rows);
  p.first_partition_size = frame.compressed_header_bytes;
  p.header_bytes = frame.uncompressed_header_bytes;

  DwordWriter w(buffers_.batch_words());
  emit_mi_flush(w);
  emit_hcp_pipe_mode_select(w, kHcpCodecVp9);
  for (uint32_t id = 0; id < 4; ++id) emit_hcp_surface_state(w, buffers_.layout(), id == 0 ? 0 : id + 1);
  emit_vp9_pipe_buf_addr_state(w, b);
  emit_hcp_ind_obj_base_addr_state(w, b.bitstream, b.bitstream_bytes, b.mocs);
  const uint32_t segments = frame.seg.enabled ? vp9::MAX_SEGMENTS : 1;
  for (uint32_t s = 0; s < segments; ++s) {
    Vp9Segment seg;
    seg.id = s;
    seg.reference_enabled = frame.seg.active(static_cast<int>(s), vp9::SEG_LVL_REF_FRAME);
    seg.reference = seg.reference_enabled ? static_cast<uint32_t>(frame.seg.feature_data[s][vp9::SEG_LVL_REF_FRAME]) & 3u : 0;
    seg.reference_skipped = frame.seg.active(static_cast<int>(s), vp9::SEG_LVL_SKIP);
    for (uint32_t r = 0; r < 4; ++r)
      for (uint32_t m = 0; m < 2; ++m) seg.filter_level[r][m] = frame.lf_adj_level[s][r][m];
    seg.luma_dc = static_cast<uint32_t>(frame.dequant[s][0]);
    seg.luma_ac = static_cast<uint32_t>(frame.dequant[s][1]);
    seg.chroma_dc = static_cast<uint32_t>(frame.dequant[s][2]);
    seg.chroma_ac = static_cast<uint32_t>(frame.dequant[s][3]);
    emit_vp9_segment_state(w, seg, frame.filter_level != 0, intra);
  }
  emit_vp9_pic_state(w, p);
  emit_vp9_bsd_object(w, static_cast<uint32_t>(frame.data.size() - frame.uncompressed_header_bytes), frame.uncompressed_header_bytes);
  emit_batch_end(w);
  if (!w.ok() || !buffers_.run(ArrayView<const uint32_t>(buffers_.batch_words().data(), w.count()))) return false;

  // What the next frame's flags depend on; the motion vector buffers swap places unless this was a key frame.
  have_last_ = true;
  last_show_ = frame.info.show_frame;
  last_intra_only_ = intra_only;
  last_width_ = width;
  last_height_ = height;
  last_frame_type_ = key ? 0 : 1;
  if (!key) {
    const uint32_t t = mv_current_;
    mv_current_ = mv_last_;
    mv_last_ = t;
  }
  return true;
}

// The decoded surface is tiled NV12: read it into the planes of 8-bit samples, then the cropped frame into the CPU decoder's 16-bit planes.
void IntelVp9Decoder::read_back(const vp9::OffloadFrame& frame, int surface) {
  const uint32_t w = buffers_.width(), h = buffers_.height(), cw = w / 2, ch = h / 2;
  host_planes_.clear();
  if (!host_planes_.put_zeros(uint64_t{w} * h + 2 * uint64_t{cw} * ch)) return;
  ByteView all = host_planes_.writable_view();
  ByteView y = all.first(uint64_t{w} * h), u = all.subview(uint64_t{w} * h, uint64_t{cw} * ch), v = all.subview(uint64_t{w} * h + uint64_t{cw} * ch, uint64_t{cw} * ch);
  if (!buffers_.read_back(surface, y, w, u, cw, v, cw, 0, 1)) return;
  ByteView planes[3] = {y, u, v};
  const uint32_t strides[3] = {w, cw, cw};
  for (int p = 0; p < 3; ++p)
    for (int row = 0; row < frame.plane_height[p]; ++row)
      for (int x = 0; x < frame.plane_width[p]; ++x)
        frame.plane[p][uint64_t{static_cast<uint32_t>(row)} * frame.stride[p] + static_cast<uint32_t>(x)] = planes[p][uint64_t{static_cast<uint32_t>(row)} * strides[p] + static_cast<uint32_t>(x)];
}

bool IntelVp9Decoder::decode_frame(const vp9::OffloadFrame& frame) {
  const int current = pick_surface();
  bool ok = current >= 0 && run_frame(frame, current);
  if (ok) {
    read_back(frame, current);
    ++decoded_;
  } else {
    ++failed_;
    if (current >= 0) buffers_.grey(current);
  }
  // The reference slots the frame refreshes hold it from now on, decoded or not (a damaged frame is what the stream's later frames would refer to).
  if (current >= 0) {
    surface_width_[current] = static_cast<uint32_t>(frame.info.width);
    surface_height_[current] = static_cast<uint32_t>(frame.info.height);
    for (unsigned i = 0; i < 8; ++i)
      if (frame.refresh_frame_flags & (1 << i)) ref_surface_[i] = current;
  }
  return ok;
}

SubmitResult IntelVp9Decoder::submit(const VideoPacket& packet) {
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

bool IntelVp9Decoder::receive(VideoFrame* frame) {
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

VideoFormat IntelVp9Decoder::output_format() const {
  VideoFormat f = cpu_.output_format();
  f.pixel_format = config_.output;
  return f;
}

VideoCodecStatistics IntelVp9Decoder::statistics() const {
  VideoCodecStatistics s = stats_;
  s.errors += failed_;
  return s;
}

}  // namespace hardware
