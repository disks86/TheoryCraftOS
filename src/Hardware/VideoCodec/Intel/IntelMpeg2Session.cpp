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
#include "Hardware/VideoCodec/Intel/IntelMpeg2Session.hpp"

namespace hardware {

namespace {
constexpr uint32_t kMaxSlices = 1024;
constexpr uint64_t kBitstreamPadding = 64;   // zeros behind the last slice, for what the engine reads ahead
}  // namespace

IntelMpeg2Decoder::IntelMpeg2Decoder(allocators::IAllocator& allocator, intel_gt::VcsDriver& driver, const DecoderConfig& config)
    : allocator_(allocator), config_(config), buffers_(allocator, driver), cpu_(allocator), host_bits_(allocator), out_(allocator) {}

IntelMpeg2Decoder::~IntelMpeg2Decoder() = default;

bool IntelMpeg2Decoder::prepare() {
  if (!config_.codec_config.empty()) return false;   // (MPEG-2 has no configuration record: the sequence header is in the stream)
  if (!buffers_.lease()) return false;
  cpu_.set_offload(this);
  return true;
}

bool IntelMpeg2Decoder::accepts(const mpeg2::SequenceHeader& seq) {
  if (!buffers_.leased()) return false;
  if (!buffers_.engine_ready()) return buffers_.is_open();   // a wedged engine still owns the stream it was decoding; a stream that never started can go to the CPU
  if (seq.chroma_format != 1 || seq.mb_width <= 0 || seq.mb_height <= 0) return false;
  return static_cast<uint32_t>(seq.mb_width) <= IntelEngineBuffers::kMaxMbsPerSide && static_cast<uint32_t>(seq.mb_height) <= IntelEngineBuffers::kMaxMbsPerSide;
}

// The slices as bitstream objects: where each one's macroblocks are in the bitstream buffer, where it starts in the picture and where the next one does.
bool IntelMpeg2Decoder::build_slices(const mpeg2::OffloadPicture& picture, ArrayView<intel_mfx::Mpeg2SliceJob> jobs, uint64_t* count) {
  const uint64_t n = picture.slices.size();
  if (n == 0 || n > jobs.size() || picture.slice_info.size() != n) return false;
  const uint32_t width = static_cast<uint32_t>(picture.seq->mb_width), frame_rows = static_cast<uint32_t>(picture.seq->mb_height);
  const bool field = picture.pic->structure != mpeg2::kFramePicture;
  const uint32_t rows = field ? frame_rows / 2 : frame_rows;
  uint64_t base = 0;
  for (uint64_t k = 0; k < n; ++k) {
    const ConstByteView slice = picture.slices[k];
    const mpeg2::SliceInfo& info = picture.slice_info[k];
    const uint64_t skip = info.mb_bit_offset >> 3;
    if (info.mb_row < 0 || static_cast<uint32_t>(info.mb_row) >= rows || info.mb_col < 0 || static_cast<uint32_t>(info.mb_col) >= width || skip >= slice.size() ||
        info.quantiser_scale_code <= 0 || base > UINT32_MAX - slice.size())
      return false;
    intel_mfx::Mpeg2SliceJob& j = jobs[k];
    j = intel_mfx::Mpeg2SliceJob();
    j.data_offset = static_cast<uint32_t>(base + skip);
    j.data_size = static_cast<uint32_t>(slice.size() - skip);
    j.bit_offset = info.mb_bit_offset & 7;
    j.hpos = static_cast<uint32_t>(info.mb_col);
    j.vpos = static_cast<uint32_t>(info.mb_row);
    j.quantiser_scale_code = static_cast<uint32_t>(info.quantiser_scale_code);
    base += slice.size();
  }
  for (uint64_t k = 0; k < n; ++k) {
    intel_mfx::Mpeg2SliceJob& j = jobs[k];
    j.last = k + 1 == n;
    if (j.last) {
      j.next_vpos = rows;
      j.next_hpos = 0;
    } else {
      j.next_vpos = jobs[k + 1].vpos;
      j.next_hpos = jobs[k + 1].hpos;
    }
    const int64_t first = int64_t{j.vpos} * width + j.hpos, end = int64_t{j.next_vpos} * width + j.next_hpos;
    if (end <= first || end - first > 255) return false;
    j.mb_count = static_cast<uint32_t>(end - first);
  }
  *count = n;
  return true;
}

bool IntelMpeg2Decoder::decode_picture(const mpeg2::OffloadPicture& picture) {
  using namespace intel_mfx;
  if (!picture.seq || !picture.pic || !buffers_.engine_ready()) {
    ++failed_;
    return false;
  }
  const mpeg2::SequenceHeader& seq = *picture.seq;
  const mpeg2::PictureHeader& pic = *picture.pic;
  const uint32_t width = static_cast<uint32_t>(seq.mb_width) * 16, height = static_cast<uint32_t>(seq.mb_height) * 16;
  if (!buffers_.is_open() || buffers_.width() != width || buffers_.height() != height) {
    if (!buffers_.open(width, height)) {
      ++failed_;
      return false;
    }
  }
  // The pictures: this one and the ones it predicts from.
  uint32_t current = 0, forward = 0, backward = 0;
  bool have_forward = false, have_backward = false;
  if (!buffers_.surface(picture.slot, &current)) {
    ++failed_;
    return false;
  }
  if (picture.forward_slot >= 0) have_forward = buffers_.surface(picture.forward_slot, &forward);
  if (picture.backward_slot >= 0) have_backward = buffers_.surface(picture.backward_slot, &backward);
  if ((picture.forward_slot >= 0 && !have_forward) || (picture.backward_slot >= 0 && !have_backward)) {
    ++failed_;
    return false;
  }
  // The reference array, as the reference driver builds it (mpeg2_set_reference_surfaces): two entries for the frame (top) references, two more for the bottom field
  // references when the picture may predict by field. An absent reference repeats the first.
  AvcBuffers b;
  const bool second_field = picture.second_field && pic.structure != mpeg2::kFramePicture;
  uint64_t refs[4] = {0, 0, 0, 0};
  auto fill = [&](uint32_t base, bool current_for_bottom) {
    uint32_t n = 0;
    if (pic.type == mpeg2::kPictureP) {
      if (second_field && (pic.structure == mpeg2::kBottomField) == current_for_bottom) refs[base + n++] = current;   // (the first field of its own frame)
      if (have_forward) refs[base + n++] = forward;
    } else if (pic.type == mpeg2::kPictureB) {
      if (have_forward) refs[base + n++] = forward;
      if (have_backward) refs[base + n++] = backward;
    }
    while (n < 2) {
      refs[base + n] = refs[base];
      ++n;
    }
  };
  fill(0, true);
  if (!pic.frame_pred_frame_dct) fill(2, false);
  for (int i = 0; i < 4; ++i) b.reference_pictures[i] = refs[i];
  uint32_t bsd_mpc = 0;
  if (!buffers_.scratch(0, uint64_t{static_cast<uint32_t>(seq.mb_width)} * 96, &bsd_mpc)) {
    ++failed_;
    return false;
  }
  b.current_picture = current;
  b.deblocking_enabled = false;
  b.bsd_mpc_row_store = bsd_mpc;

  Mpeg2SliceJob jobs[kMaxSlices];
  uint64_t slice_count = 0;
  if (!build_slices(picture, ArrayView<Mpeg2SliceJob>(jobs, kMaxSlices), &slice_count)) {
    ++failed_;
    buffers_.grey(picture.slot);
    return false;
  }
  // The slices, one after the other, each with its start code, in the bitstream buffer.
  host_bits_.clear();
  for (const ConstByteView& slice : picture.slices)
    if (!host_bits_.append(slice)) {
      ++failed_;
      return false;
    }
  uint32_t bitstream = 0;
  if (!host_bits_.put_zeros(kBitstreamPadding) || !buffers_.upload_bitstream(host_bits_.view(), &bitstream)) {
    ++failed_;
    return false;
  }

  b.bitstream = bitstream;
  Mpeg2Picture p;
  p.width_in_mbs = static_cast<uint32_t>(seq.mb_width);
  p.height_in_mbs = static_cast<uint32_t>(seq.mb_height);
  p.picture_coding_type = static_cast<uint32_t>(pic.type);
  for (uint32_t d = 0; d < 2; ++d)
    for (uint32_t c = 0; c < 2; ++c) p.f_code[d][c] = static_cast<uint32_t>(pic.f_code[d][c]);
  p.intra_dc_precision = static_cast<uint32_t>(pic.intra_dc_precision);
  p.picture_structure = static_cast<uint32_t>(pic.structure);
  p.top_field_first = pic.top_field_first;
  p.frame_pred_frame_dct = pic.frame_pred_frame_dct;
  p.concealment_motion_vectors = pic.concealment_motion_vectors;
  p.q_scale_type = pic.q_scale_type;
  p.intra_vlc_format = pic.intra_vlc_format;
  p.alternate_scan = pic.alternate_scan;

  DwordWriter w(buffers_.batch_words());
  emit_mi_flush(w);
  emit_pipe_mode_select(w, kFormatMpeg2, /*post=*/false, /*pre=*/true);
  emit_surface_state(w, width, height, buffers_.layout());
  emit_pipe_buf_addr_state(w, b);
  emit_bsp_buf_base_addr_state(w, b);
  emit_mpeg2_pic_state(w, p);
  emit_qm_state(w, kQmMpegIntra, ConstByteView(picture.intra_matrix, 64));
  emit_qm_state(w, kQmMpegNonIntra, ConstByteView(picture.non_intra_matrix, 64));
  emit_ind_obj_base_addr_state(w, b.bitstream, b.mocs);
  for (uint64_t k = 0; k < slice_count; ++k) emit_mpeg2_bsd_object(w, jobs[k]);
  emit_batch_end(w);
  if (!w.ok() || !buffers_.run(ArrayView<const uint32_t>(buffers_.batch_words().data(), w.count()))) {
    ++failed_;
    buffers_.grey(picture.slot);
    return false;
  }
  const bool field = pic.structure != mpeg2::kFramePicture;
  const uint32_t first = field && pic.structure == mpeg2::kBottomField ? 1 : 0, step = field ? 2 : 1;
  if (!buffers_.read_back(picture.slot, picture.plane[0], static_cast<uint32_t>(picture.stride[0]), picture.plane[1], static_cast<uint32_t>(picture.stride[1]), picture.plane[2],
                          static_cast<uint32_t>(picture.stride[2]), first, step)) {
    ++failed_;
    return false;
  }
  ++decoded_;
  return true;
}

SubmitResult IntelMpeg2Decoder::submit(const VideoPacket& packet) {
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

bool IntelMpeg2Decoder::receive(VideoFrame* frame) {
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

VideoFormat IntelMpeg2Decoder::output_format() const {
  VideoFormat f = cpu_.output_format();
  f.pixel_format = config_.output;
  return f;
}

VideoCodecStatistics IntelMpeg2Decoder::statistics() const {
  VideoCodecStatistics s = stats_;
  s.errors += failed_;
  return s;
}

}  // namespace hardware
