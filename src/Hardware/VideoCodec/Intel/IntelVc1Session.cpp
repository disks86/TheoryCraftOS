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
#include "Hardware/VideoCodec/Intel/IntelVc1Session.hpp"

namespace hardware {

namespace {
constexpr uint32_t kMaxUnits = 256;
constexpr uint64_t kBitstreamPadding = 64;   // zeros behind the last unit, for what the engine reads ahead
constexpr int kSkippedPicture = 4;
// The scale of a B picture's fraction by its index (the BFRACTION table), as the engine takes it (b_picture_scale_factor).
constexpr uint8_t kBPictureScaleFactor[21] = {128, 85, 170, 64, 192, 51, 102, 153, 204, 43, 215, 37, 74, 111, 148, 185, 222, 32, 96, 160, 224};

bool is_start_code(ConstByteView unit) { return unit.size() >= 4 && unit[0] == 0 && unit[1] == 0 && unit[2] == 1; }

// Where the first macroblock is in the unit as it is stored (with emulation prevention bytes), given where it is in the unit without them: the reference driver's
// gen8_mfd_vc1_get_macroblock_bit_offset. The byte right behind a prevention byte that ends the header is made a plain zero, as it does, so that the engine starts behind it.
uint32_t stored_bit_offset(ByteView unit, uint32_t bits) {
  auto at = [&](uint64_t i) -> uint8_t {
    uint8_t v = 0xFF;
    unit.try_get(i, &v);
    return v;
  };
  auto prevention = [&](uint64_t j) { return at(j) == 0 && at(j + 1) == 0 && at(j + 2) == 3 && at(j + 3) < 4; };
  const uint64_t header_bytes = bits / 8;
  if (header_bytes == 0) return bits;
  uint64_t i = 0, j = 0;
  for (; i + 1 < header_bytes; ++i, ++j)
    if (prevention(j)) {
      ++i;
      j += 2;
    }
  if (i + 1 == header_bytes) {
    if (prevention(j)) {
      unit[j + 2] = 0;
      ++j;
    }
    ++j;
  }
  return static_cast<uint32_t>(8 * j + bits % 8);
}
}  // namespace

IntelVc1Decoder::IntelVc1Decoder(allocators::IAllocator& allocator, intel_gt::VcsDriver& driver, const DecoderConfig& config)
    : allocator_(allocator), config_(config), buffers_(allocator, driver), cpu_(allocator), host_bits_(allocator), host_plane_(allocator), out_(allocator) {
  for (int& t : picture_type_) t = -1;
}

IntelVc1Decoder::~IntelVc1Decoder() = default;

bool IntelVc1Decoder::prepare() {
  if (!buffers_.lease()) return false;
  cpu_.set_offload(this);
  if (!config_.codec_config.empty() && !cpu_.configure(config_.codec_config, config_.width, config_.height)) {
    cpu_.set_offload(nullptr);
    buffers_.release_lease();
    return false;
  }
  return true;
}

void IntelVc1Decoder::reset() {
  cpu_.reset();
  for (int& t : picture_type_) t = -1;
}

bool IntelVc1Decoder::accepts(const vc1::SequenceHeader& seq) {
  if (!buffers_.leased()) return false;
  if (!buffers_.engine_ready()) return buffers_.is_open();   // a wedged engine still owns the stream it was decoding; a stream that never started can go to the CPU
  if (seq.interlace || seq.mb_width <= 0 || seq.mb_height <= 0) return false;
  return static_cast<uint32_t>(seq.mb_width) <= IntelEngineBuffers::kMaxMbsPerSide && static_cast<uint32_t>(seq.mb_height) <= IntelEngineBuffers::kMaxMbsPerSide;
}

// The bit planes as the engine reads them: a nibble per macroblock, the first macroblock of a pair in the low half, a row after another, each row a whole number of bytes.
// Bit 0 of a nibble is the first plane of the picture's kind, bit 1 the second, bit 2 the third (P: direct, skip, motion vector type; B: direct, skip; I and BI: field
// transform, AC prediction, overlap flags). A skipped picture has the skip bit set everywhere.
bool IntelVc1Decoder::build_bitplane(const vc1::OffloadPicture& picture, uint32_t* address) {
  const uint32_t width = static_cast<uint32_t>(picture.seq->mb_width), height = static_cast<uint32_t>(picture.seq->mb_height);
  const uint32_t row_bytes = (width + 1) / 2;
  host_plane_.clear();
  if (!host_plane_.put_zeros(uint64_t{row_bytes} * height)) return false;
  ByteView out = host_plane_.writable_view();
  const vc1::PictureHeader& pic = *picture.pic;
  ArrayView<const uint8_t> planes[3];
  if (pic.type == vc1::kPictureP) {
    planes[1] = picture.skip_plane;
    planes[2] = picture.mv_type_plane;
  } else if (pic.type == vc1::kPictureB) {
    planes[0] = picture.direct_plane;
    planes[1] = picture.skip_plane;
  } else {
    planes[1] = picture.acpred_plane;
    planes[2] = picture.overflags_plane;
  }
  for (uint32_t y = 0; y < height; ++y)
    for (uint32_t x = 0; x < width; ++x) {
      uint32_t v = 0;
      for (uint32_t k = 0; k < 3; ++k) {
        uint8_t bit = 0;
        if (!planes[k].empty() && !planes[k].try_get(uint64_t{y} * width + x, &bit)) return false;
        v |= uint32_t{bit & 1u} << k;
      }
      const uint64_t index = uint64_t{y} * row_bytes + x / 2;
      out[index] = static_cast<uint8_t>(out[index] | (v << (4 * (x & 1))));
    }
  return buffers_.scratch(2, host_plane_.size(), address) && buffers_.upload_scratch(2, host_plane_.view());
}

bool IntelVc1Decoder::decode_picture(const vc1::OffloadPicture& picture) {
  using namespace intel_mfx;
  if (!picture.seq || !picture.pic || !buffers_.engine_ready()) {
    ++failed_;
    return false;
  }
  const vc1::SequenceHeader& seq = *picture.seq;
  const vc1::PictureHeader& pic = *picture.pic;
  const uint32_t width = static_cast<uint32_t>(seq.mb_width) * 16, height = static_cast<uint32_t>(seq.mb_height) * 16;
  const uint32_t width_mbs = static_cast<uint32_t>(seq.mb_width), height_mbs = static_cast<uint32_t>(seq.mb_height);
  if (!buffers_.is_open() || buffers_.width() != width || buffers_.height() != height) {
    if (!buffers_.open(width, height)) {
      ++failed_;
      return false;
    }
    for (int& t : picture_type_) t = -1;
  }
  if (picture.slot < 0 || picture.slot >= vc1::kPoolSize) {
    ++failed_;
    return false;
  }
  uint32_t current = 0, forward = 0, backward = 0;
  if (!buffers_.surface(picture.slot, &current)) {
    ++failed_;
    return false;
  }
  const bool have_forward = picture.forward_slot >= 0 && buffers_.surface(picture.forward_slot, &forward);
  const bool have_backward = picture.backward_slot >= 0 && buffers_.surface(picture.backward_slot, &backward);
  if ((picture.forward_slot >= 0 && !have_forward) || (picture.backward_slot >= 0 && !have_backward)) {
    ++failed_;
    return false;
  }
  // A skipped picture repeats the one before it: its surface is a copy of that one's (the CPU decoder has made the planes).
  if (pic.skipped) {
    if (!have_forward && picture.anchor_slot[1] < 0) {
      ++failed_;
      return false;
    }
    const int source = picture.anchor_slot[1];
    if (source < 0 || !buffers_.surface(source, &forward) || !buffers_.copy_surface(source, picture.slot)) {
      ++failed_;
      return false;
    }
    picture_type_[picture.slot] = kSkippedPicture;
    ++decoded_;
    return true;
  }
  const bool intra = pic.type == vc1::kPictureI || pic.type == vc1::kPictureBI;
  if ((!intra && pic.type != vc1::kPictureB && !have_forward) || (pic.type == vc1::kPictureB && !have_forward)) {
    ++failed_;
    return false;
  }
  const uint32_t picture_dmv_bytes = 128u * height_mbs * 64;
  uint32_t dmv_write = 0, dmv_read = 0;
  if (pic.type == vc1::kPictureP && !buffers_.picture_buffer(picture.slot, picture_dmv_bytes, &dmv_write)) {
    ++failed_;
    return false;
  }
  bool dmv_valid = false;
  if (pic.type == vc1::kPictureB && have_backward && picture_type_[picture.backward_slot] == vc1::kPictureP && buffers_.picture_buffer(picture.backward_slot, picture_dmv_bytes, &dmv_read))
    dmv_valid = true;

  // Buffers: the references (the engine's reference array repeats forward, backward, forward, backward...), the output and the scratch memory.
  AvcBuffers b;
  const uint64_t refs[2] = {have_forward ? forward : 0, have_backward ? backward : (have_forward ? forward : 0)};
  for (uint32_t i = 0; i < kMaxAvcReferenceFrames; ++i) b.reference_pictures[i] = refs[i & 1];
  uint32_t intra_row = 0, deblock_row = 0, bsd_mpc = 0;
  if (!buffers_.scratch(0, uint64_t{width_mbs} * 64, &intra_row) || !buffers_.scratch(1, uint64_t{width_mbs} * 7 * 64, &deblock_row) ||
      !buffers_.scratch(3, uint64_t{width_mbs} * 96, &bsd_mpc)) {
    ++failed_;
    return false;
  }
  b.current_picture = current;
  b.deblocking_enabled = seq.loop_filter;
  b.intra_row_store = intra_row;
  b.deblocking_row_store = deblock_row;
  b.bsd_mpc_row_store = bsd_mpc;
  const bool planes_present = !picture.skip_plane.empty() || !picture.mv_type_plane.empty() || !picture.direct_plane.empty() || !picture.acpred_plane.empty() ||
                              !picture.overflags_plane.empty();
  if (planes_present) {
    uint32_t address = 0;
    if (!build_bitplane(picture, &address)) {
      ++failed_;
      return false;
    }
    b.bitplane_read = address;
  }

  // The frame and its slices, one after the other (each without its start code), in the bitstream buffer; each unit's first macroblock position.
  const uint64_t unit_count = picture.units.size();
  if (unit_count == 0 || unit_count > kMaxUnits || picture.unit_info.size() != unit_count) {
    ++failed_;
    return false;
  }
  const bool advanced = seq.profile == vc1::kAdvanced;
  Vc1BsdJob jobs[kMaxUnits];
  host_bits_.clear();
  uint64_t base = 0;
  for (uint64_t k = 0; k < unit_count; ++k) {
    ConstByteView unit = picture.units[k];
    if (is_start_code(unit)) unit = unit.subview(4);
    if (unit.empty() || !host_bits_.append(unit)) {
      ++failed_;
      return false;
    }
    const vc1::UnitInfo& info = picture.unit_info[k];
    uint32_t bits = info.mb_bit_offset;
    if (advanced) bits = stored_bit_offset(host_bits_.writable_view().subview(base, unit.size()), bits);
    if ((bits >> 3) >= unit.size() || base > UINT32_MAX - unit.size() || info.start_row < 0) {
      ++failed_;
      buffers_.grey(picture.slot);
      return false;
    }
    Vc1BsdJob& j = jobs[k];
    j.data_offset = static_cast<uint32_t>(base + (bits >> 3));
    j.data_size = static_cast<uint32_t>(unit.size() - (bits >> 3));
    j.bit_offset = bits & 7;
    j.first_row = static_cast<uint32_t>(info.start_row);
    base += unit.size();
  }
  for (uint64_t k = 0; k < unit_count; ++k) jobs[k].next_row = k + 1 < unit_count ? jobs[k + 1].first_row : height_mbs;
  uint32_t bitstream = 0;
  if (!host_bits_.put_zeros(kBitstreamPadding) || !buffers_.upload_bitstream(host_bits_.view(), &bitstream)) {
    ++failed_;
    return false;
  }
  b.bitstream = bitstream;

  // The picture state, from the headers.
  Vc1Picture p;
  p.width_in_mbs = width_mbs;
  p.height_in_mbs = height_mbs;
  p.direct_mv_valid = dmv_valid;
  p.implicit_quantizer = seq.quantizer_mode == vc1::kQuantFrameImplicit;
  p.rounding_control = static_cast<uint32_t>(pic.rnd);
  p.sync_marker = seq.resync_marker;
  p.advanced = advanced;
  p.loop_filter = seq.loop_filter;
  p.picture_type = static_cast<uint32_t>(pic.type);   // (I, P, B, BI: the engine's numbers)
  p.pic_quantizer_scale = static_cast<uint32_t>(pic.pq);
  p.half_qp = pic.halfpq;
  p.pic_quantizer_type = pic.pquantizer;
  p.fast_uvmc = seq.fastuvmc;
  p.extended_mv_range = static_cast<uint32_t>(pic.mvrange);
  if (pic.type == vc1::kPictureB && pic.bfraction_index >= 0 && pic.bfraction_index < 21) p.scale_factor = kBPictureScaleFactor[pic.bfraction_index];
  // Overlap smoothing is on for the pictures the standard smooths (the quantizer, the profile and the picture type decide).
  if (seq.overlap) {
    if (advanced) {
      if (pic.type == vc1::kPictureP && pic.pq >= 9) p.overlap = true;
      if (intra) p.overlap = pic.pq >= 9 || pic.condover == vc1::kCondOverAll || pic.condover == vc1::kCondOverSelect;
    } else {
      p.overlap = pic.pq >= 9 && pic.type != vc1::kPictureB;
    }
  }
  p.conditional_overlap = pic.condover == vc1::kCondOverAll ? 2 : (pic.condover == vc1::kCondOverSelect ? 3 : 0);
  if (pic.type == vc1::kPictureP || pic.type == vc1::kPictureB) {
    const int mode = pic.mv_mode == vc1::kMvIntensityComp ? pic.mv_mode2 : pic.mv_mode;
    p.unified_mv_mode = mode == vc1::kMv1Mv ? 1 : mode == vc1::kMv1MvHpel ? 2 : mode == vc1::kMv1MvHpelBilin ? 3 : 0;
    p.interpolation_mode = (mode == vc1::kMv1MvHpelBilin ? 8u : 0u) | (seq.fastuvmc ? 1u : 0u);
  }
  // The quantizer changes within the picture (dquant): the configuration and the edges they apply to.
  p.alt_pq = static_cast<uint32_t>(pic.altpq);
  if (seq.dquant == 2) {
    p.alt_pquant_config = 1;
    p.alt_pquant_edge_mask = 0xF;
  } else if (seq.dquant == 1) {
    if (!pic.dquantfrm) {
      p.alt_pq = 0;
    } else {
      p.alt_pquant_config = 1;
      switch (pic.dqprofile) {
        case vc1::kDqAllMbs:
          p.alt_pquant_config = pic.dqbilevel ? 3 : 2;
          break;
        case vc1::kDqFourEdges: p.alt_pquant_edge_mask = 0xF; break;
        case vc1::kDqDoubleEdges: p.alt_pquant_edge_mask = pic.dqsbedge == 3 ? 0x9u : (0x3u << pic.dqsbedge); break;
        case vc1::kDqSingleEdge: p.alt_pquant_edge_mask = 0x1u << pic.dqsbedge; break;
        default: break;
      }
    }
  }
  p.bitplane_present = planes_present;
  p.raw_mv_type = pic.mv_type_raw;
  p.raw_skip = pic.skip_raw;
  p.raw_direct = pic.direct_raw;
  p.raw_overflags = pic.overflags_raw;
  p.raw_ac_pred = pic.acpred_raw;
  p.mv_table = static_cast<uint32_t>(pic.mv_table_index);
  p.cbp_table = static_cast<uint32_t>(pic.cbptab);
  p.intra_transform_dc_table = static_cast<uint32_t>(pic.dc_table_index);
  p.transform_ac_codingset_idx1 = static_cast<uint32_t>(pic.c_ac_table_index);
  p.trans_ac_y = static_cast<uint32_t>(intra ? pic.y_ac_table_index : pic.c_ac_table_index);
  if (!intra) {
    // Without variable-sized transform coding every block uses the 8x8 transform; the engine wants it said so.
    p.mb_level_transform_type = seq.vstransform ? pic.ttmbf : true;
    const int tt = seq.vstransform ? pic.ttfrm : vc1::kTt8x8;
    p.frame_level_transform_type = tt == vc1::kTt8x4 ? 1 : tt == vc1::kTt4x8 ? 2 : tt == vc1::kTt4x4 ? 3 : 0;
  }

  DwordWriter w(buffers_.batch_words());
  emit_mi_flush(w);
  emit_pipe_mode_select(w, kFormatVc1, /*post=*/seq.loop_filter, /*pre=*/!seq.loop_filter);
  emit_surface_state(w, width, height, buffers_.layout());
  emit_pipe_buf_addr_state(w, b);
  emit_bsp_buf_base_addr_state(w, b);
  emit_vc1_pic_state(w, p);
  emit_vc1_pred_pipe_state(w, picture.ic_forward && (pic.type == vc1::kPictureP || pic.type == vc1::kPictureB), static_cast<uint32_t>(picture.ic_scale), static_cast<uint32_t>(picture.ic_shift));
  emit_vc1_directmode_state(w, dmv_write, dmv_valid ? dmv_read : 0, b.mocs);
  emit_ind_obj_base_addr_state(w, b.bitstream, b.mocs);
  for (uint64_t k = 0; k < unit_count; ++k) emit_vc1_bsd_object(w, jobs[k]);
  emit_batch_end(w);
  if (!w.ok() || !buffers_.run(ArrayView<const uint32_t>(buffers_.batch_words().data(), w.count()))) {
    ++failed_;
    buffers_.grey(picture.slot);
    return false;
  }
  picture_type_[picture.slot] = pic.type;
  if (!buffers_.read_back(picture.slot, picture.plane[0], static_cast<uint32_t>(picture.stride[0]), picture.plane[1], static_cast<uint32_t>(picture.stride[1]), picture.plane[2],
                          static_cast<uint32_t>(picture.stride[2]), 0, 1)) {
    ++failed_;
    return false;
  }
  ++decoded_;
  return true;
}

SubmitResult IntelVc1Decoder::submit(const VideoPacket& packet) {
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

bool IntelVc1Decoder::receive(VideoFrame* frame) {
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

VideoFormat IntelVc1Decoder::output_format() const {
  VideoFormat f = cpu_.output_format();
  f.pixel_format = config_.output;
  return f;
}

VideoCodecStatistics IntelVc1Decoder::statistics() const {
  VideoCodecStatistics s = stats_;
  s.errors += failed_;
  return s;
}

}  // namespace hardware
