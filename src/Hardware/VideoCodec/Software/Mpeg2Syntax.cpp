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


// MPEG-2 video decoding, part 1: start codes, headers, slices and macroblocks (ISO/IEC 13818-2, clauses 6 and 7). Reconstruction is in Mpeg2Recon.cpp.
#include "Hardware/VideoCodec/Software/Mpeg2Core.hpp"

namespace hardware {

Mpeg2Decoder::~Mpeg2Decoder() = default;

bool Mpeg2Decoder::decode(ConstByteView data, int64_t pts_us) {
  if (!core_) core_ = UniquePointer<mpeg2::Core>::make(allocator_, allocator_);
  return core_ && core_->decode(data, pts_us);
}
bool Mpeg2Decoder::receive(VideoFrame* frame) { return core_ && core_->receive(frame); }
void Mpeg2Decoder::flush() {
  if (core_) core_->flush();
}
void Mpeg2Decoder::reset() {
  if (core_) core_->reset();
}
VideoFormat Mpeg2Decoder::output_format() const { return core_ ? core_->output_format() : VideoFormat(); }
bool Mpeg2Decoder::ready() const { return core_ && core_->ready(); }
void Mpeg2Decoder::set_offload(mpeg2::IOffload* offload) {
  if (!core_) core_ = UniquePointer<mpeg2::Core>::make(allocator_, allocator_);
  if (core_) core_->set_offload(offload);
}
void Mpeg2Decoder::set_frame_observer(mpeg2::IFrameObserver* observer) {
  if (!core_) core_ = UniquePointer<mpeg2::Core>::make(allocator_, allocator_);
  if (core_) core_->set_frame_observer(observer);
}

namespace mpeg2 {

namespace {
#include "Hardware/VideoCodec/Software/Mpeg2Tables.inc"
}  // namespace

namespace {
// Builds a lookup table indexed by the next `max_bits` bits: (symbol << 5) | length, 0 for a bit pattern that is no code.
bool make_lut(allocators::IAllocator& a, Mem<uint16_t>* lut, const uint16_t* codes, const uint8_t* bits, int count, int max_bits) {
  if (!lut->alloc(a, uint64_t{1} << max_bits)) return false;
  ArrayView<uint16_t> table(lut->data(), lut->size());
  for (int sym = 0; sym < count; ++sym) {
    const int len = bits[sym];
    if (len == 0 || len > max_bits) continue;
    const uint32_t first = static_cast<uint32_t>(codes[sym]) << (max_bits - len);
    for (uint32_t i = 0; i < (1u << (max_bits - len)); ++i) table[first + i] = static_cast<uint16_t>((sym << 5) | len);
  }
  return true;
}

// The motion vector residual range of a motion_code: ISO 13818-2 7.6.3.1.
constexpr int kMaxFCode = 9;
}  // namespace

Core::Core(allocators::IAllocator& allocator) : allocator_(allocator), slices_(allocator) {}

bool Core::build_vlcs() {
  if (vlcs_ready_) return true;
  uint16_t escape_codes[34];
  uint8_t escape_bits[34];
  for (int i = 0; i < 33; ++i) {
    escape_codes[i] = kAddressIncrementCode[i];
    escape_bits[i] = kAddressIncrementBits[i];
  }
  escape_codes[33] = kAddressEscapeCode;
  escape_bits[33] = kAddressEscapeBits;
  if (!make_lut(allocator_, &lut_address_, escape_codes, escape_bits, 34, 11) || !make_lut(allocator_, &lut_pattern_, kCodedBlockPatternCode, kCodedBlockPatternBits, 64, 9) ||
      !make_lut(allocator_, &lut_motion_, kMotionCodeCode, kMotionCodeBits, 17, 11) || !make_lut(allocator_, &lut_dc_luma_, kDcSizeLumaCode, kDcSizeLumaBits, 12, 9) ||
      !make_lut(allocator_, &lut_dc_chroma_, kDcSizeChromaCode, kDcSizeChromaBits, 12, 10) || !make_lut(allocator_, &lut_dct_[0], kDctTable0Code, kDctTable0Bits, 113, 16) ||
      !make_lut(allocator_, &lut_dct_[1], kDctTable1Code, kDctTable1Bits, 113, 16))
    return false;
  vlcs_ready_ = true;
  return true;
}

// ---- the output ---------------------------------------------------------------------------------------------------------------------

VideoFormat Core::output_format() const {
  if (!have_sequence_) return VideoFormat();
  return VideoFormat{static_cast<uint32_t>(seq_.horizontal_size), static_cast<uint32_t>(seq_.vertical_size), PixelFormat::I420};
}

Picture* Core::find_free_picture() {
  for (Picture& p : pool_) {
    if (p.in_use && &p != cur_ && &p != anchor_[0] && &p != anchor_[1] && &p != held_ && &p != returned_ && !p.output_pending) p.in_use = false;
  }
  for (Picture& p : pool_)
    if (!p.in_use) return &p;
  return nullptr;
}

void Core::output_picture(Picture* p) {
  if (!p || p->output_pending) return;
  if (queue_count_ >= kOutputQueue) {
    // The caller does not take pictures: the oldest one is lost.
    queue_[0]->output_pending = false;
    for (int i = 1; i < queue_count_; ++i) queue_[i - 1] = queue_[i];
    --queue_count_;
  }
  p->output_pending = true;
  queue_[queue_count_++] = p;
}

bool Core::receive(VideoFrame* frame) {
  if (queue_count_ == 0) return false;
  Picture* p = queue_[0];
  for (int i = 1; i < queue_count_; ++i) queue_[i - 1] = queue_[i];
  --queue_count_;
  p->output_pending = false;
  returned_ = p;
  frame->format = output_format();
  for (int c = 0; c < 3; ++c) {
    frame->plane[c] = p->plane[c].data();
    frame->stride[c] = static_cast<uint32_t>(p->stride[c]);
  }
  frame->pts_us = p->pts;
  frame->keyframe = p->type == kPictureI;
  frame->limited_range = true;
  return true;
}

void Core::flush() {
  if (picture_open_) finish_picture();
  if (first_field_done_ && cur_) {
    // A first field without its second: the frame goes out as it is.
    first_field_done_ = false;
    if (cur_->type == kPictureB) output_picture(cur_);
    else {
      anchor_[0] = anchor_[1];
      anchor_[1] = cur_;
      if (held_) output_picture(held_);
      held_ = cur_;
    }
    cur_ = nullptr;
  }
  if (held_) output_picture(held_);
  held_ = nullptr;
}

void Core::reset() {
  picture_open_ = false;
  first_field_done_ = false;
  cur_ = nullptr;
  anchor_[0] = anchor_[1] = nullptr;
  held_ = nullptr;
  returned_ = nullptr;
  for (int i = 0; i < queue_count_; ++i) queue_[i]->output_pending = false;
  queue_count_ = 0;
  for (Picture& p : pool_) p.in_use = false;
  need_intra_ = true;
  have_picture_header_ = false;
}

// ---- headers ---------------------------------------------------------------------------------------------------------------------

// 6.2.2.1 sequence_header()
bool Core::parse_sequence_header(BitReader& br) {
  SequenceHeader s = seq_;
  s.horizontal_size = static_cast<int>(br.get(12));
  s.vertical_size = static_cast<int>(br.get(12));
  s.aspect_ratio = static_cast<int>(br.get(4));
  s.frame_rate_code = static_cast<int>(br.get(4));
  br.skip(18 + 1 + 10 + 1);   // bit_rate, marker, vbv_buffer_size, constrained_parameters_flag
  // A sequence with no extension is MPEG-1: the extension sets these (and the sequence is not accepted without it).
  s.progressive_sequence = true;
  s.chroma_format = 1;
  s.low_delay = false;
  s.profile_and_level = 0;
  if (br.bit()) {
    for (int i = 0; i < 64; ++i) s.intra_matrix[kZigzagScan[i]] = static_cast<uint8_t>(br.get(8));
  } else {
    for (int i = 0; i < 64; ++i) s.intra_matrix[i] = kDefaultIntraMatrix[i];
  }
  if (br.bit()) {
    for (int i = 0; i < 64; ++i) s.non_intra_matrix[kZigzagScan[i]] = static_cast<uint8_t>(br.get(8));
  } else {
    for (int i = 0; i < 64; ++i) s.non_intra_matrix[i] = 16;
  }
  if (br.error() || s.horizontal_size == 0 || s.vertical_size == 0) return false;
  s.valid = false;   // until the sequence extension arrives
  seq_ = s;
  have_sequence_ = false;
  return true;
}

// 6.2.2.2 extension_and_user_data: the extensions that matter (sequence, quantiser matrix, picture coding).
bool Core::parse_extension(BitReader& br) {
  const int id = static_cast<int>(br.get(4));
  if (id == 1) {   // sequence_extension
    if (seq_.horizontal_size == 0) return false;
    seq_.profile_and_level = static_cast<int>(br.get(8));
    seq_.progressive_sequence = br.bit() != 0;
    seq_.chroma_format = static_cast<int>(br.get(2));
    const int h_ext = static_cast<int>(br.get(2)), v_ext = static_cast<int>(br.get(2));
    br.skip(12 + 1 + 8);   // bit_rate_extension, marker, vbv_buffer_size_extension
    seq_.low_delay = br.bit() != 0;
    br.skip(2 + 5);        // frame_rate_extension_n, _d
    if (br.error()) return false;
    seq_.horizontal_size |= h_ext << 12;
    seq_.vertical_size |= v_ext << 12;
    if (seq_.chroma_format != 1 || seq_.horizontal_size > kMaxMbWidth * 16 || seq_.vertical_size > kMaxMbHeight * 16) return false;   // 4:2:0 only
    seq_.mb_width = (seq_.horizontal_size + 15) / 16;
    seq_.mb_height = seq_.progressive_sequence ? (seq_.vertical_size + 15) / 16 : 2 * ((seq_.vertical_size + 31) / 32);
    seq_.valid = true;
    have_sequence_ = true;
    if (offload_) offloaded_ = offload_->accepts(seq_);
    return true;
  }
  if (id == 3) {   // quant_matrix_extension: replaces the matrices
    if (br.bit()) {
      for (int i = 0; i < 64; ++i) seq_.intra_matrix[kZigzagScan[i]] = static_cast<uint8_t>(br.get(8));
    }
    if (br.bit()) {
      for (int i = 0; i < 64; ++i) seq_.non_intra_matrix[kZigzagScan[i]] = static_cast<uint8_t>(br.get(8));
    }
    return !br.error();
  }
  if (id == 8) {   // picture_coding_extension
    if (!have_picture_header_) return false;
    for (int s = 0; s < 2; ++s)
      for (int t = 0; t < 2; ++t) pic_.f_code[s][t] = static_cast<int>(br.get(4));
    pic_.intra_dc_precision = static_cast<int>(br.get(2));
    pic_.structure = static_cast<int>(br.get(2));
    pic_.top_field_first = br.bit() != 0;
    pic_.frame_pred_frame_dct = br.bit() != 0;
    pic_.concealment_motion_vectors = br.bit() != 0;
    pic_.q_scale_type = br.bit() != 0;
    pic_.intra_vlc_format = br.bit() != 0;
    pic_.alternate_scan = br.bit() != 0;
    pic_.repeat_first_field = br.bit() != 0;
    pic_.chroma_420_type = br.bit() != 0;
    pic_.progressive_frame = br.bit() != 0;
    pic_.has_extension = true;
    return !br.error() && pic_.structure >= 1 && pic_.structure <= 3;
  }
  return true;   // other extensions (display, scalable, ...) are of no use here
}

// 6.2.3 picture_header()
bool Core::parse_picture_header(BitReader& br) {
  PictureHeader p;
  p.temporal_reference = static_cast<int>(br.get(10));
  p.type = static_cast<int>(br.get(3));
  br.skip(16);   // vbv_delay
  if (p.type == kPictureP || p.type == kPictureB) {
    br.skip(1);   // full_pel_forward_vector (MPEG-1)
    br.skip(3);   // forward_f_code (MPEG-1; the extension has the MPEG-2 ones)
    if (p.type == kPictureB) br.skip(4);
  }
  if (br.error() || p.type < kPictureI || p.type > kPictureB) return false;
  pic_ = p;
  have_picture_header_ = true;
  return true;
}

// ---- pictures ---------------------------------------------------------------------------------------------------------------------

bool Core::start_picture() {
  if (!have_sequence_ || !have_picture_header_ || !pic_.has_extension || !build_vlcs()) return false;
  const bool field = pic_.structure != kFramePicture;
  if (pic_.type != kPictureI && need_intra_) return false;
  // The second field of the frame being decoded?
  if (field && first_field_done_ && cur_ && pic_.structure != first_field_structure_ && cur_->width == seq_.mb_width * 16) {
    first_field_done_ = false;
    second_field_ = true;
  } else {
    second_field_ = false;
    if (first_field_done_ && cur_) {
      // The second field never came: the frame as it is goes through the output rules.
      first_field_done_ = false;
      if (cur_->type != kPictureB) {
        anchor_[0] = anchor_[1];
        anchor_[1] = cur_;
        if (held_) output_picture(held_);
        held_ = cur_;
      } else {
        output_picture(cur_);
      }
      cur_ = nullptr;
    }
    // Anchors needed by the picture type.
    if (pic_.type == kPictureP && !anchor_[1]) return false;
    if (pic_.type == kPictureB && (!anchor_[0] || !anchor_[1])) return false;
    // A new size drops what was decoded in the old one.
    if (sequence_.horizontal_size != seq_.horizontal_size || sequence_.vertical_size != seq_.vertical_size || sequence_.mb_height != seq_.mb_height) {
      for (Picture& p : pool_) {
        if (p.output_pending) continue;
        p.in_use = false;
      }
      anchor_[0] = anchor_[1] = nullptr;
      held_ = nullptr;
    }
    Picture* p = find_free_picture();
    if (!p) return false;
    const int w = seq_.mb_width * 16, h = seq_.mb_height * 16;
    if (p->width != w || p->height != h || !p->plane[0].data()) {
      if (!p->plane[0].alloc(allocator_, uint64_t(w) * h) || !p->plane[1].alloc(allocator_, uint64_t(w / 2) * (h / 2)) || !p->plane[2].alloc(allocator_, uint64_t(w / 2) * (h / 2)))
        return false;
      p->width = w;
      p->height = h;
      p->stride[0] = w;
      p->stride[1] = p->stride[2] = w / 2;
    }
    for (int c = 0; c < 3; ++c)
      for (uint64_t i = 0; i < p->plane[c].size(); ++i) p->plane[c][i] = 128;
    p->in_use = true;
    p->output_pending = false;
    p->pts = pts_;
    p->type = pic_.type;
    p->top_field_first = pic_.top_field_first;
    p->progressive_frame = pic_.progressive_frame;
    p->repeat_first_field = pic_.repeat_first_field;
    p->order = decode_counter_++;
    cur_ = p;
    if (field) {
      first_field_structure_ = pic_.structure;
    }
    sequence_ = seq_;
  }
  picture_open_ = true;
  slice_count_ = 0;
  slices_.clear();
  return true;
}

void Core::finish_picture() {
  picture_open_ = false;
  if (!cur_) return;
  if (slice_count_ > 0 || offloaded_) {
    OffloadPicture o;
    o.seq = &sequence_;
    o.pic = &pic_;
    o.slot = static_cast<int>(cur_ - pool_);
    if (pic_.type == kPictureP && anchor_[1]) o.forward_slot = static_cast<int>(anchor_[1] - pool_);
    if (pic_.type == kPictureB) {
      if (anchor_[0]) o.forward_slot = static_cast<int>(anchor_[0] - pool_);
      if (anchor_[1]) o.backward_slot = static_cast<int>(anchor_[1] - pool_);
    }
    for (int i = 0; i < 2; ++i)
      if (anchor_[i]) o.anchor_slot[i] = static_cast<int>(anchor_[i] - pool_);
    const uint8_t* scan = pic_.alternate_scan ? kAlternateScan : kZigzagScan;
    for (int i = 0; i < 64; ++i) {
      o.intra_matrix_scan[i] = sequence_.intra_matrix[scan[i]];
      o.non_intra_matrix_scan[i] = sequence_.non_intra_matrix[scan[i]];
    }
    o.second_field = second_field_;
    o.slices = slices_.view();
    for (int c = 0; c < 3; ++c) {
      o.plane[c] = ByteView(cur_->plane[c].data(), cur_->plane[c].size());
      o.stride[c] = cur_->stride[c];
    }
    if (offloaded_) offload_->decode_picture(o);   // (a picture it cannot decode stays grey)
    else if (observer_) observer_->picture_decoded(o);
  }
  const bool field = pic_.structure != kFramePicture;
  if (field && !first_field_done_ && first_field_structure_ == pic_.structure) {
    first_field_done_ = true;   // the second field is expected; the frame is not complete
    if (pic_.type == kPictureI) need_intra_ = false;
    return;
  }
  if (pic_.type == kPictureI) need_intra_ = false;
  Picture* p = cur_;
  if (pic_.type == kPictureB) {
    output_picture(p);
  } else if (seq_.low_delay) {
    anchor_[0] = anchor_[1];
    anchor_[1] = p;
    output_picture(p);
  } else {
    anchor_[0] = anchor_[1];
    anchor_[1] = p;
    if (held_) output_picture(held_);
    held_ = p;
  }
  cur_ = nullptr;
}

// ---- the data ---------------------------------------------------------------------------------------------------------------------

bool Core::decode(ConstByteView data, int64_t pts) {
  bool any = false;
  pts_ = pts;
  // Cut the data at its start codes (00 00 01); each unit goes from one start code to the next.
  uint64_t i = 0;
  const uint64_t n = data.size();
  auto next_code = [&](uint64_t from) {
    for (uint64_t k = from; k + 3 <= n; ++k)
      if (data[k] == 0 && data[k + 1] == 0 && data[k + 2] == 1) return k;
    return n;
  };
  i = next_code(0);
  while (i < n) {
    const uint64_t end = next_code(i + 3);
    ConstByteView unit = data.subview(i, end - i);
    if (unit.size() >= 4) any |= handle_unit(unit, pts);
    i = end;
  }
  // A call holds whole pictures: the last one is complete now (a frame in field pictures waits for its second field).
  if (picture_open_) finish_picture();
  return any;
}

bool Core::handle_unit(ConstByteView unit, int64_t) {
  const int code = unit[3];
  BitReader br(unit.subview(4));
  if (code == 0xB3) {
    if (picture_open_) finish_picture();
    return parse_sequence_header(br);
  }
  if (code == 0xB5) return parse_extension(br);
  if (code == 0xB8) {
    if (picture_open_) finish_picture();
    return true;
  }
  if (code == 0xB7) {
    if (picture_open_) finish_picture();
    return true;
  }
  if (code == 0x00) {
    if (picture_open_) finish_picture();
    have_picture_header_ = false;
    return parse_picture_header(br);
  }
  if (code >= 0x01 && code <= 0xAF) {
    if (!picture_open_ && !start_picture()) return false;
    return decode_slice(unit, code - 1);
  }
  return true;   // user data, sequence display extension, ...
}

}  // namespace mpeg2
}  // namespace hardware
