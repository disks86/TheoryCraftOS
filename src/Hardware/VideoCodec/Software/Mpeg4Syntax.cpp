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


// MPEG-4 Part 2 and H.263 decoding, part 1: packets, planes, video packets and groups of blocks, macroblocks and blocks, and the predictions made while parsing
// (DC and AC coefficients, motion vectors, direct mode). ISO/IEC 14496-2 clauses 6 and 7, ITU-T H.263 clause 5. Reconstruction is in Mpeg4Recon.cpp.
#include "Hardware/VideoCodec/Software/Mpeg4Core.hpp"

namespace hardware {

Mpeg4Decoder::~Mpeg4Decoder() = default;

bool Mpeg4Decoder::decode(ConstByteView data, int64_t pts_us) {
  if (!core_) core_ = UniquePointer<mpeg4::Core>::make(allocator_, allocator_);
  return core_ && core_->decode(data, pts_us);
}
bool Mpeg4Decoder::receive(VideoFrame* frame) { return core_ && core_->receive(frame); }
void Mpeg4Decoder::flush() {
  if (core_) core_->flush();
}
void Mpeg4Decoder::reset() {
  if (core_) core_->reset();
}
VideoFormat Mpeg4Decoder::output_format() const { return core_ ? core_->output_format() : VideoFormat(); }
bool Mpeg4Decoder::ready() const { return core_ && core_->ready(); }

namespace mpeg4 {

namespace {
#include "Hardware/VideoCodec/Software/Mpeg4Tables.inc"

constexpr int kMcbpcLen = 9, kCbpyLen = 6, kMvLen = 12, kDcLumaLen = 11, kDcChromaLen = 12, kCoefficientLen = 12, kBTypeLen = 4, kReversibleLen = 15;   // the longest code of each table
constexpr int kEscape = 102;
constexpr int kStuffingIntra = 8, kStuffingInter = 20;
constexpr int8_t kDquant[4] = {-1, -2, 1, 2};

// A lookup table indexed by the next `max_bits` bits: (symbol << 5) | length, 0 for a bit pattern that starts no code.
bool make_lut(allocators::IAllocator& a, Mem<uint16_t>* lut, ArrayView<const uint16_t> codes, ArrayView<const uint8_t> bits, int max_bits) {
  if (!lut->alloc(a, uint64_t{1} << max_bits)) return false;
  ArrayView<uint16_t> table(lut->data(), lut->size());
  for (uint64_t sym = 0; sym < codes.size() && sym < bits.size(); ++sym) {
    const int len = bits[sym];
    if (len == 0 || len > max_bits) continue;
    const uint32_t first = static_cast<uint32_t>(codes[sym]) << (max_bits - len);
    for (uint32_t i = 0; i < (1u << (max_bits - len)); ++i) table[first + i] = static_cast<uint16_t>((sym << 5) | static_cast<uint64_t>(len));
  }
  return true;
}

int clamp_int(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

// 7.4.3.2 Table 7-1: the DC scaler of luma and chroma blocks.
int luma_dc_scale(int q) { return q < 5 ? 8 : q < 9 ? 2 * q : q < 25 ? q + 8 : 2 * q - 16; }
int chroma_dc_scale(int q) { return q < 5 ? 8 : q < 25 ? (q + 13) / 2 : q - 6; }

int rounded_div(int a, int b) { return (a >= 0 ? a + (b >> 1) : a - (b >> 1)) / b; }

// The value of `bits` bits as a two's complement number.
int sign_extend(uint32_t value, int bits) {
  const uint32_t sign = 1u << (bits - 1);
  value &= (sign << 1) - 1;
  return static_cast<int>(value ^ sign) - static_cast<int>(sign);
}

// 6.3.5: the resync marker after the stuffing at a byte boundary, as 16 bits from a position `n` bits into a byte: a zero, ones to the boundary, then zeros.
void clear_macroblock(Mb& mb) {
  mb.intra = mb.skipped = mb.ac_pred = mb.field_dct = mb.mcsel = false;
  mb.cbp = 0;
  mb.mv_type = kMv16x16;
  mb.dir = kForward;
  for (auto& d : mb.mv)
    for (auto& v : d) v[0] = v[1] = 0;
  for (auto& d : mb.field_select) d[0] = d[1] = 0;
  for (int& l : mb.last) l = -1;
  for (auto& b : mb.block) ArrayView<int16_t>(b).fill(0);
  mb.aic_left = false;
  mb.b_mode = kBBidirectional;
  mb.mvd[0] = mb.mvd[1] = 0;
  mb.cbpb = 0;
  for (int& l : mb.b_last) l = -1;
  for (auto& b : mb.b_block) ArrayView<int16_t>(b).fill(0);
}

constexpr uint16_t kResyncPrefix[8] = {0x7F00, 0x7E00, 0x7C00, 0x7800, 0x7000, 0x6000, 0x4000, 0x0000};
}  // namespace

Core::Core(allocators::IAllocator& allocator) : allocator_(allocator) {
  for (int i = 0; i < 64; ++i) {
    scan_tables_[0][i] = kZigzag[i];
    scan_tables_[2][i] = kAlternateVertical[i];
    const int v = kAlternateVertical[i];
    scan_tables_[1][i] = static_cast<uint8_t>((v & 7) * 8 + (v >> 3));   // the alternate horizontal scan: the transpose
  }
  const CoefficientTable* tables[2] = {&kIntraCoefficients, &kInterCoefficients};
  for (int t = 0; t < 2; ++t) {
    const CoefficientTable& table = *tables[t];
    for (int i = 0; i < kEscape; ++i) {
      const int last = i >= table.last_start ? 1 : 0, run = table.run[i], level = table.level[i];
      if (level > max_level_[t][last][run]) max_level_[t][last][run] = static_cast<uint8_t>(level);
      if (run > max_run_[t][last][level]) max_run_[t][last][level] = static_cast<uint8_t>(run);
    }
  }
}

bool Core::build_vlcs() {
  if (vlcs_ready_) return true;
  if (!make_lut(allocator_, &lut_intra_mcbpc_, kIntraMcbpcCode, kIntraMcbpcBits, kMcbpcLen) ||
      !make_lut(allocator_, &lut_inter_mcbpc_, kInterMcbpcCode, kInterMcbpcBits, kMcbpcLen) || !make_lut(allocator_, &lut_cbpy_, kCbpyCode, kCbpyBits, kCbpyLen) ||
      !make_lut(allocator_, &lut_mv_, kMvCode, kMvBits, kMvLen) || !make_lut(allocator_, &lut_dc_[0], kDcLumaCode, kDcLumaBits, kDcLumaLen) ||
      !make_lut(allocator_, &lut_dc_[1], kDcChromaCode, kDcChromaBits, kDcChromaLen) ||
      !make_lut(allocator_, &lut_coef_[0], kIntraCoefficients.code, kIntraCoefficients.bits, kCoefficientLen) ||
      !make_lut(allocator_, &lut_coef_[1], kInterCoefficients.code, kInterCoefficients.bits, kCoefficientLen) ||
      !make_lut(allocator_, &lut_btype_, kBTypeCode, kBTypeBits, kBTypeLen) ||
      !make_lut(allocator_, &lut_rvlc_[0], kIntraReversible.code, kIntraReversible.bits, kReversibleLen) ||
      !make_lut(allocator_, &lut_rvlc_[1], kInterReversible.code, kInterReversible.bits, kReversibleLen) ||
      !make_lut(allocator_, &lut_aic_, kAicCoefficients.code, kAicCoefficients.bits, kCoefficientLen))
    return false;
  vlcs_ready_ = true;
  return true;
}

int Core::read_vlc(BitReader& br, const Mem<uint16_t>& lut, int max_bits) {
  const uint16_t entry = ArrayView<const uint16_t>(lut.data(), lut.size())[br.peek(max_bits)];
  if (entry == 0) return -1;
  br.skip(entry & 31);
  return entry >> 5;
}

// ---- the output ---------------------------------------------------------------------------------------------------------------------

VideoFormat Core::output_format() const { return out_format_; }

Picture* Core::find_free_picture() {
  for (Picture& p : pool_) {
    if (p.in_use && &p != cur_ && &p != anchor_[0] && &p != anchor_[1] && &p != held_ && &p != returned_ && &p != awaiting_time_ && &p != cur_b_ && !p.output_pending)
      p.in_use = false;
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
  frame->format = out_format_;
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
  if (awaiting_time_) output_picture(awaiting_time_);
  awaiting_time_ = nullptr;
  if (held_) output_picture(held_);
  held_ = nullptr;
}

void Core::reset() {
  cur_ = nullptr;
  anchor_[0] = anchor_[1] = nullptr;
  held_ = nullptr;
  returned_ = nullptr;
  for (int i = 0; i < queue_count_; ++i) queue_[i]->output_pending = false;
  queue_count_ = 0;
  for (Picture& p : pool_) p.in_use = false;
  need_intra_ = true;
  awaiting_time_ = nullptr;
}

// ---- packets and planes ---------------------------------------------------------------------------------------------------------------

bool Core::decode(ConstByteView data, int64_t pts_us) {
  if (!build_vlcs()) return false;
  placeholder_expected_ = awaiting_time_ != nullptr;
  if (awaiting_time_) {
    awaiting_time_->pts = pts_us;
    output_picture(awaiting_time_);
    awaiting_time_ = nullptr;
  }
  pictures_in_packet_ = 0;
  const bool ok = decode_packet(data, pts_us);
  placeholder_expected_ = false;
  return ok;
}

bool Core::decode_packet(ConstByteView data, int64_t pts_us) {
  // An H.263 picture starts with its picture start code; an MPEG-4 packet with a start code (00 00 01), perhaps after some zero bytes.
  uint64_t first = 0;
  while (first < data.size() && data[first] == 0 && !is_short_header(data.subview(first))) ++first;
  if (first < data.size() && is_short_header(data.subview(first))) return decode_h263(data.subview(first), pts_us);
  return decode_mpeg4(data, pts_us);
}

bool Core::decode_mpeg4(ConstByteView data, int64_t pts) {
  const uint64_t n = data.size();
  auto next_start = [&](uint64_t from) {
    for (uint64_t i = from; i + 3 < n; ++i)
      if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) return i;
    return n;
  };
  bool used = false;
  uint64_t at = next_start(0);
  while (at < n) {
    const uint8_t code = data[at + 3];
    const uint64_t begin = at + 4, end = next_start(begin);
    const ConstByteView body = data.subview(begin, end - begin);
    if (code == 0xB0) {
      if (body.size() >= 1) profile_and_level_ = body[0];
    } else if (code == 0xB2) {
      // user_data: Xvid signs its streams "XviD" and its build number.
      if (body.size() >= 5 && body[0] == 'X' && body[1] == 'v' && body[2] == 'i' && body[3] == 'D' && body[4] >= '0' && body[4] <= '9') xvid_idct_ = true;
    } else if (code >= 0x20 && code <= 0x2F) {
      Vol v;
      if (parse_vol_header(body, profile_and_level_, &v) && configure(static_cast<int>(v.width), static_cast<int>(v.height))) {
        if (!have_vol_ || short_header_) low_delay_ = v.low_delay;
        vol_ = v;
        for (int i = 0; i < 64; ++i) {
          intra_matrix_[kZigzag[i]] = v.intra_matrix[i];
          inter_matrix_[kZigzag[i]] = v.non_intra_matrix[i];
        }
        have_vol_ = true;
        short_header_ = false;
        used = true;
      }
    } else if (code == 0xB6) {
      if (have_vol_ && !short_header_ && decode_vop(body, pts)) used = true;
    }
    at = end;
  }
  return used;
}

bool Core::decode_vop(ConstByteView body, int64_t pts) {
  VopHeader h;
  if (!parse_vop_header(body, vol_, &h)) return false;
  // A packed bitstream's placeholder (not coded, after a packet with two planes) stands for the B plane already decoded: its time is not the stream's.
  if (!h.coded && placeholder_expected_) return true;
  // The plane's time and the distances of direct mode, as FFmpeg computes them (decode_vop_header); the times move on even for a plane that is not coded.
  const int64_t resolution = vol_.time_increment_resolution;
  if (h.type != VopType::B) {
    last_time_base_ = time_base_;
    time_base_ += h.modulo_time_base;
    time_ = time_base_ * resolution + h.time_increment;
    pp_time_ = time_ - last_non_b_time_;
    last_non_b_time_ = time_;
  } else {
    time_ = (last_time_base_ + h.modulo_time_base) * resolution + h.time_increment;
    pb_time_ = pp_time_ - (last_non_b_time_ - time_);
    if (pp_time_ <= pb_time_ || pp_time_ <= pp_time_ - pb_time_ || pp_time_ <= 0) return true;   // out of order (after a seek): FFmpeg skips the plane
    if (t_frame_ == 0) t_frame_ = pb_time_;
    if (t_frame_ == 0) t_frame_ = 1;
    auto rdiv = [](int64_t a, int64_t b) { return (a >= 0 ? a + (b >> 1) : a - (b >> 1)) / b; };
    pp_field_time_ = (rdiv(last_non_b_time_, t_frame_) - rdiv(last_non_b_time_ - pp_time_, t_frame_)) * 2;
    pb_field_time_ = (rdiv(time_, t_frame_) - rdiv(last_non_b_time_ - pp_time_, t_frame_)) * 2;
    if (pp_field_time_ <= pb_field_time_ || pb_field_time_ <= 1) {
      pb_field_time_ = 2;
      pp_field_time_ = 4;
      if (vol_.interlaced) return true;
    }
  }
  if (!h.coded) return true;   // "repeat the previous picture": nothing new comes out (as with FFmpeg)
  PlaneParams p;
  p.type = h.type == VopType::S ? kPictureP : static_cast<int>(h.type);
  p.quant = h.quant;
  p.fcode_forward = h.fcode_forward;
  p.fcode_backward = h.fcode_backward;
  p.no_rounding = h.rounding_type;
  p.dc_threshold = kIntraDcThreshold[h.intra_dc_vlc_thr & 7];
  p.alternate_scan = h.alternate_vertical_scan;
  p.top_field_first = h.top_field_first;
  if (p.type == kPictureB) {
    low_delay_ = false;   // the layer said otherwise, wrongly
    if (!anchor_[0] || !anchor_[1]) return true;
  }
  if (need_intra_ && p.type != kPictureI) return true;
  pic_ = p;
  if (h.type == VopType::S && !setup_sprite(h)) return false;
  if (!begin_picture(pts)) return false;
  BitReader br(body);
  br.skip(static_cast<int>(h.header_bits));
  decode_macroblocks(br);
  end_picture();
  return true;
}

bool Core::decode_h263(ConstByteView data, int64_t pts) {
  ShortHeader h;
  if (!parse_short_header(data, have_h263_ ? &h263_ : nullptr, &h)) return false;
  if (!configure(static_cast<int>(h.width), static_cast<int>(h.height))) return false;
  h263_ = h;
  have_h263_ = true;
  if (!short_header_) {
    short_header_ = true;
    have_vol_ = false;
    vol_ = Vol();
    vol_.width = h.width;
    vol_.height = h.height;
    vol_.low_delay = true;
    low_delay_ = true;
  }
  // Version 2 streams may have B pictures (Annex O), which come after the reference picture shown after them: their reference pictures wait for the next.
  if (h.plus) low_delay_ = false;
  PlaneParams p;
  p.type = h.b_picture ? kPictureB : h.inter ? kPictureP : kPictureI;
  p.short_header = true;
  p.quant = h.quant;
  p.continuous_presence = h.continuous_presence;
  p.no_rounding = h.inter && h.no_rounding;
  p.long_vectors = h.long_vectors;
  p.umv_plus = h.umv_plus;
  p.obmc = h.advanced_prediction;
  p.aic = h.advanced_intra;
  p.deblocking = h.deblocking;
  p.slices = h.slice_structured;
  p.aiv = h.alternative_inter_vlc;
  p.modified_quant = h.modified_quant;
  p.first_mb = static_cast<int>(h.first_mb);
  // The temporal distances (G.4, O.5.2): in picture clock ticks, modulo the reference's range.
  const int modulo = h.custom_pcf ? 1024 : 256;
  auto distance = [&](int to, int from) { const int d = ((to - from) % modulo + modulo) % modulo; return d ? d : 1; };
  const int tr = h.tr, tr_last = tr_last_;
  tr_last_ = tr;
  pb_mode_ = h.pb;
  dbquant_ = h.dbquant;
  if (h.pb) {
    trd_ = tr_last < 0 ? h.trb + 1 : distance(tr, tr_last);
    trb_ = h.trb;
    pb_p_delay_us_ = static_cast<int64_t>(trd_ > trb_ ? trd_ - trb_ : 1) * h.tick_us;
  } else if (h.b_picture) {
    trb_ = distance(tr, tr_anchor_[0]);
    trd_ = distance(tr_anchor_[1], tr_anchor_[0]);
    if (!anchor_[0] || !anchor_[1]) return true;
  }
  if (need_intra_ && p.type != kPictureI) return true;
  if (p.first_mb >= mb_count_) return false;
  pic_ = p;
  if (!begin_picture(pts)) return false;
  BitReader br(data);
  br.skip(static_cast<int>(h.header_bits));
  decode_macroblocks(br);
  if (!h.b_picture) {
    tr_anchor_[0] = tr_anchor_[1];
    tr_anchor_[1] = tr;
  }
  end_picture();
  return true;
}

bool Core::configure(int width, int height) {
  if (width <= 0 || height <= 0) return false;
  const int mb_w = (width + 15) / 16, mb_h = (height + 15) / 16;
  if (mb_w > kMaxMbWidth || mb_h > kMaxMbHeight) return false;
  if (width == width_ && height == height_ && mb_count_ > 0) return true;
  const uint64_t count = static_cast<uint64_t>(mb_w) * static_cast<uint64_t>(mb_h);
  if (!mb_packet_.alloc(allocator_, count) || !mb_intra_.alloc(allocator_, count) || !mb_qscale_.alloc(allocator_, count) || !dc_[0].alloc(allocator_, count * 4) ||
      !dc_[1].alloc(allocator_, count) || !dc_[2].alloc(allocator_, count) || !ac_[0].alloc(allocator_, count * 4 * 16) || !ac_[1].alloc(allocator_, count * 16) ||
      !ac_[2].alloc(allocator_, count * 16) || !partition_.alloc(allocator_, count) ||
      !bvec_.alloc(allocator_, count)) {
    mb_count_ = 0;
    width_ = height_ = 0;
    return false;
  }
  width_ = width;
  height_ = height;
  mb_w_ = mb_w;
  mb_h_ = mb_h;
  mb_count_ = mb_w * mb_h;
  out_format_ = VideoFormat{static_cast<uint32_t>(width), static_cast<uint32_t>(height), PixelFormat::I420};
  reset();
  return true;
}

bool Core::begin_picture(int64_t pts) {
  Picture* p = find_free_picture();
  if (!p) return false;
  const int w = mb_w_ * 16, h = mb_h_ * 16;
  if (p->width != w || p->height != h || p->mbs.size() != static_cast<uint64_t>(mb_count_)) {
    p->width = p->height = 0;
    if (!p->plane[0].alloc(allocator_, static_cast<uint64_t>(w) * h) || !p->plane[1].alloc(allocator_, static_cast<uint64_t>(w / 2) * (h / 2)) ||
        !p->plane[2].alloc(allocator_, static_cast<uint64_t>(w / 2) * (h / 2)) || !p->mbs.alloc(allocator_, static_cast<uint64_t>(mb_count_)))
      return false;
    p->width = w;
    p->height = h;
    p->stride[0] = w;
    p->stride[1] = p->stride[2] = w / 2;
  }
  p->in_use = true;
  p->output_pending = false;
  p->type = pic_.type;
  p->pts = pts;
  cur_ = p;
  cur_b_ = nullptr;
  if (pic_.short_header && pb_mode_ && pic_.type == kPictureP) {
    // A PB-frame: its B part is a picture of its own, shown before the P part.
    Picture* b = find_free_picture();
    if (!b) return false;
    if (b->width != w || b->height != h || b->mbs.size() != static_cast<uint64_t>(mb_count_)) {
      b->width = b->height = 0;
      if (!b->plane[0].alloc(allocator_, static_cast<uint64_t>(w) * h) || !b->plane[1].alloc(allocator_, static_cast<uint64_t>(w / 2) * (h / 2)) ||
          !b->plane[2].alloc(allocator_, static_cast<uint64_t>(w / 2) * (h / 2)) || !b->mbs.alloc(allocator_, static_cast<uint64_t>(mb_count_)))
        return false;
      b->width = w;
      b->height = h;
      b->stride[0] = w;
      b->stride[1] = b->stride[2] = w / 2;
    }
    b->in_use = true;
    b->output_pending = false;
    b->type = kPictureB;
    b->pts = pts;
    p->pts = pts + pb_p_delay_us_;
    cur_b_ = b;
  }
  if (pic_.short_header && (pb_mode_ || pic_.type == kPictureB)) ArrayView<BVectors>(bvec_.data(), bvec_.size()).fill(BVectors{});
  ArrayView<int32_t>(mb_packet_.data(), mb_packet_.size()).fill(-1);
  qscale_ = pic_.quant;
  packet_start_ = 0;
  for (auto& d : last_mv_)
    for (auto& f : d) f[0] = f[1] = 0;
  return true;
}

void Core::end_picture() {
  Picture* p = cur_;
  cur_ = nullptr;
  if (!p) return;
  if (cur_b_) {   // a PB-frame's B part goes out after the picture before it and before its P part
    if (held_) output_picture(held_);
    held_ = nullptr;
    output_picture(cur_b_);
    cur_b_ = nullptr;
  }
  const bool extra = pictures_in_packet_++ > 0;
  if (p->type == kPictureB) {
    if (extra && !awaiting_time_) awaiting_time_ = p;   // (packed: its time comes with the next packet)
    else output_picture(p);
    return;
  }
  anchor_[0] = anchor_[1];
  anchor_[1] = p;
  need_intra_ = false;
  if (low_delay_) {
    if (held_) output_picture(held_);
    held_ = nullptr;
    output_picture(p);
  } else {
    if (held_) output_picture(held_);
    held_ = p;
  }
}

// ---- video packets and groups of blocks ---------------------------------------------------------------------------------------------

int Core::resync_prefix_length() const {
  if (pic_.type == kPictureI) return 16;
  if (pic_.type == kPictureP) return pic_.fcode_forward + 15;
  int f = pic_.fcode_forward > pic_.fcode_backward ? pic_.fcode_forward : pic_.fcode_backward;
  return (f > 2 ? f : 2) + 15;
}

// After a macroblock: 0 when another one follows, 1 when a resync marker (after its stuffing) does, -1 when the plane ends here (only stuffing is left). As FFmpeg's
// mpeg4_is_resync, macroblock stuffing in front of it is consumed.
int Core::check_resync(BitReader& br) {
  const int type_bits = pic_.type == kPictureI ? 1 : 2;   // the length of the stuffing code after 8 zero bits: I 9 bits, P 10 (with not_coded)
  uint32_t v = br.peek(16);
  while (v <= 0xFF && pic_.type != kPictureB && (v >> (8 - type_bits)) == 1 && br.bits_left() > 16) {
    br.skip(8 + type_bits);
    v = br.peek(16);
  }
  const uint64_t pos = br.position();
  if (br.bits_left() <= 8) {
    const uint32_t w = (v >> 8) | (0x7Fu >> (7 - (pos & 7)));
    if (w == 0x7F) return -1;
    return 0;
  }
  if (v != kResyncPrefix[pos & 7]) return 0;
  BitReader look = br;
  look.skip(1);
  look.align();
  int len = 0;
  while (len < 32 && !look.bit()) ++len;
  return len >= resync_prefix_length() ? 1 : 0;
}

// 6.2.5.2 video_packet_header(), at the resync marker (byte aligned). The fields repeated after header_extension_code are skipped, as FFmpeg skips them.
bool Core::video_packet_header(BitReader& br, int* mb_index) {
  if (br.bits_left() < 20) return false;
  int len = 0;
  while (len < 32 && !br.bit()) ++len;
  if (len != resync_prefix_length()) return false;
  int mb_bits = 1;
  while ((1 << mb_bits) < mb_count_) ++mb_bits;
  const int mb_num = static_cast<int>(br.get(mb_bits));
  if (mb_num <= 0 || mb_num >= mb_count_) return false;
  const int q = static_cast<int>(br.get(5));
  if (q) qscale_ = q;
  if (br.bit()) {   // header_extension_code
    for (int i = 0; br.bit(); ++i)
      if (i > 4096 || br.error()) return false;
    br.skip(1);
    br.skip(static_cast<int>(vol_.time_increment_bits));
    br.skip(1);
    br.skip(2);   // vop_coding_type
    br.skip(3);   // intra_dc_vlc_thr
    if (pic_.type != kPictureI) br.skip(3);
    if (pic_.type == kPictureB) br.skip(3);
  }
  if (br.error()) return false;
  *mb_index = mb_num;
  return true;
}

// H.263 5.2: a group of blocks header (GBSC after stuffing, GN, [GSBI], GFID, GQUANT). False for anything else, a picture start code among them.
bool Core::gob_header(BitReader& br, int* mb_index) {
  if (br.peek(16) != 0) return false;
  br.skip(16);
  int zeros = 0;
  while (zeros < 16 && !br.bit()) ++zeros;
  if (zeros >= 16 || br.error()) return false;
  if (pic_.slices) {
    // Annex K.2: a slice header (SEPB1, MBA, [SEPB3], SQUANT, SEPB2, GFID).
    static const int kMbaMax[6] = {47, 98, 395, 1583, 6335, 9215};
    static const int kMbaLength[7] = {6, 7, 9, 11, 13, 14, 14};
    int k = 0;
    while (k < 6 && mb_count_ - 1 > kMbaMax[k]) ++k;
    if (!br.bit()) return false;
    const int mba = static_cast<int>(br.get(kMbaLength[k]));
    if (mb_count_ > 1583 && !br.bit()) return false;
    const int q = static_cast<int>(br.get(5));
    if (!br.bit()) return false;
    br.skip(2);
    if (q == 0 || mba >= mb_count_ || br.error()) return false;
    qscale_ = q;
    *mb_index = mba;
    return true;
  }
  const int gn = static_cast<int>(br.get(5));
  if (gn == 0 || gn == 31) return false;   // a picture start code or the end of the sequence
  if (pic_.continuous_presence) br.skip(2);
  br.skip(2);   // GFID
  const int q = static_cast<int>(br.get(5));
  if (q == 0 || br.error()) return false;
  const int rows = height_ <= 400 ? 1 : height_ <= 800 ? 2 : 4;
  const int first = gn * rows * mb_w_;
  if (first >= mb_count_) return false;
  qscale_ = q;
  *mb_index = first;
  return true;
}

// ---- the macroblock loop ---------------------------------------------------------------------------------------------------------------

// Damage at macroblock `index`: the macroblocks up to the next resync marker (or group of blocks) that can be found are concealed. The next macroblock to decode, or
// -1 when none is found (the rest of the plane is concealed).
int Core::recover(BitReader& br, int index) {
  int next = -1;
  br.align();
  while (br.bits_left() > 24) {
    if (br.peek(16) == 0) {
      BitReader at = br;
      int n = -1;
      if ((pic_.short_header ? gob_header(at, &n) : video_packet_header(at, &n)) && n >= index) {
        next = n;
        br = at;
        break;
      }
    }
    br.skip(8);
  }
  conceal(index, next < 0 ? mb_count_ : next);
  if (next < 0) return -1;
  packet_start_ = next;
  for (auto& d : last_mv_)
    for (auto& f : d) f[0] = f[1] = 0;
  return next;
}

void Core::decode_macroblocks(BitReader& br) {
  int index = 0;
  packet_start_ = 0;
  if (vol_.data_partitioned && !pic_.short_header && pic_.type != kPictureB) {
    // 6.2.5.2: each video packet in three parts: the modes and motion (I: the DC coefficients too), after a marker the coded patterns (P: also intra DCs),
    // then the texture of all its macroblocks.
    while (index < mb_count_) {
      int count = 0;
      if (!partitioned_packet(br, index, &count)) {
        index = recover(br, index + count);
        if (index < 0) return;
        continue;
      }
      index += count;
      if (index >= mb_count_) return;
      const int r = check_resync(br);
      int next = -1;
      if (r > 0) {
        br.skip(1);
        br.align();
        if (!video_packet_header(br, &next) || next < index) next = -1;
      }
      if (next < 0) {
        if (r < 0) {
          conceal(index, mb_count_);
          return;
        }
        index = recover(br, index);   // (a packet must end at a resync marker)
        if (index < 0) return;
        continue;
      }
      if (next > index) conceal(index, next);
      index = next;
      packet_start_ = next;
    }
    return;
  }
  if (pic_.first_mb > 0) {   // (a slice-structured picture whose first slice is not at the start)
    conceal(0, pic_.first_mb);
    index = packet_start_ = pic_.first_mb;
  }
  while (index < mb_count_) {
    mb_x_ = index % mb_w_;
    mb_y_ = index / mb_w_;
    if (pic_.type == kPictureB && mb_x_ == 0) {
      for (auto& d : last_mv_)
        for (auto& f : d) f[0] = f[1] = 0;
    }
    Mb& mb = mb_;
    bool ok = pic_.short_header ? (pic_.type == kPictureB ? macroblock_h263_b(br, mb) : macroblock_h263(br, mb)) : macroblock_mpeg4(br, mb);
    if (ok && br.error()) ok = false;   // ran past the end of the plane
    if (!ok) {
      index = recover(br, index);
      if (index < 0) return;
      continue;
    }
    mb_packet_[static_cast<uint64_t>(index)] = packet_start_;
    mb_intra_[static_cast<uint64_t>(index)] = mb.intra ? 1 : 0;
    mb_qscale_[static_cast<uint64_t>(index)] = static_cast<uint8_t>(qscale_);
    record_macroblock(mb);
    if (pic_.obmc && pic_.type == kPictureP && !mb.intra && mb_x_ + 1 < mb_w_) preview_next(br);
    reconstruct(mb);
    ++index;
    if (index >= mb_count_) break;
    int next = index;
    if (pic_.short_header) {
      if (br.bits_left() < 16) {
        if (br.peek(static_cast<int>(br.bits_left() < 1 ? 1 : br.bits_left())) == 0) {
          conceal(index, mb_count_);
          return;
        }
        continue;
      }
      if (br.peek(16) != 0) continue;
      if (!gob_header(br, &next) || next < index) {
        conceal(index, mb_count_);
        return;
      }
    } else {
      const int r = check_resync(br);
      if (r == 0) continue;
      if (r < 0) {
        conceal(index, mb_count_);
        return;
      }
      br.skip(1);
      br.align();
      if (!video_packet_header(br, &next) || next < index) {
        conceal(index, mb_count_);
        return;
      }
      for (auto& d : last_mv_)
        for (auto& f : d) f[0] = f[1] = 0;
    }
    if (next > index) conceal(index, next);
    index = next;
    packet_start_ = next;
  }
}

// One video packet of a data-partitioned I or P plane starting at macroblock `start`: its `count` macroblocks are decoded and reconstructed.
bool Core::partitioned_packet(BitReader& br, int start, int* count) {
  *count = 0;
  constexpr uint32_t kDcMarker = 0x6B001, kMotionMarker = 0x1F001;   // 19 and 17 bits
  ArrayView<PartitionMb> part(partition_.data(), partition_.size());
  const bool intra_plane = pic_.type == kPictureI;
  int n = 0;
  // Partition 1.
  for (int index = start; index < mb_count_; ++index) {
    if (intra_plane ? br.peek(19) == kDcMarker : br.peek(17) == kMotionMarker) break;
    mb_x_ = index % mb_w_;
    mb_y_ = index / mb_w_;
    PartitionMb& m = part[static_cast<uint64_t>(index)];
    m = PartitionMb();
    MbInfo& info = cur_->mbs[static_cast<uint64_t>(index)];
    for (auto& v : info.mv) v[0] = v[1] = 0;
    int cbpc = 0;
    if (intra_plane) {
      do {
        cbpc = read_vlc(br, lut_intra_mcbpc_, kMcbpcLen);
        if (cbpc < 0 || br.error()) return false;
      } while (cbpc == kStuffingIntra);
      m.intra = true;
      m.cbpc = static_cast<uint8_t>(cbpc & 3);
      m.use_dc_vlc = qscale_ < pic_.dc_threshold;
      if (cbpc & 4) qscale_ = clamp_int(qscale_ + kDquant[br.get(2)], 1, 31);
      m.qscale = static_cast<uint8_t>(qscale_);
      if (m.use_dc_vlc && !partition_dc(br, m)) return false;
    } else {
      for (;;) {
        if (br.bit()) {
          m.skipped = true;
          if (pic_.gmc) {
            m.mcsel = true;
            const int16_t gx = static_cast<int16_t>(global_vector(0)), gy = static_cast<int16_t>(global_vector(1));
            for (auto& v : info.mv) {
              v[0] = gx;
              v[1] = gy;
            }
          }
          break;
        }
        cbpc = read_vlc(br, lut_inter_mcbpc_, kMcbpcLen);
        if (cbpc < 0) return false;
        if (cbpc != kStuffingInter) break;
        if (br.error()) return false;
      }
      if (!m.skipped) {
        m.cbpc = static_cast<uint8_t>(cbpc & 3);
        m.dquant = (cbpc & 8) != 0;
        m.intra = (cbpc & 4) != 0;
        if (!m.intra) {
          if (pic_.gmc && (cbpc & 16) == 0) m.mcsel = br.bit() != 0;
          if (m.mcsel) {
            const int16_t gx = static_cast<int16_t>(global_vector(0)), gy = static_cast<int16_t>(global_vector(1));
            for (auto& v : info.mv) {
              v[0] = gx;
              v[1] = gy;
            }
          } else if ((cbpc & 16) == 0) {
            int px, py, mx, my;
            predict_motion(0, &px, &py);
            if (!read_motion(br, px, pic_.fcode_forward, &mx) || !read_motion(br, py, pic_.fcode_forward, &my)) return false;
            for (auto& v : info.mv) {
              v[0] = static_cast<int16_t>(mx);
              v[1] = static_cast<int16_t>(my);
            }
          } else {
            m.four = true;
            for (int i = 0; i < 4; ++i) {
              int px, py, mx, my;
              predict_motion(i, &px, &py);
              if (!read_motion(br, px, pic_.fcode_forward, &mx) || !read_motion(br, py, pic_.fcode_forward, &my)) return false;
              info.mv[i][0] = static_cast<int16_t>(mx);
              info.mv[i][1] = static_cast<int16_t>(my);
            }
          }
        }
      }
    }
    if (br.error()) return false;
    // (The neighbours of the next macroblocks: in this packet, intra or not.)
    mb_packet_[static_cast<uint64_t>(index)] = packet_start_;
    mb_intra_[static_cast<uint64_t>(index)] = m.intra ? 1 : 0;
    ++n;
  }
  if (n == 0) return false;
  if (intra_plane) {
    while (br.peek(9) == 1) br.skip(9);   // macroblock stuffing
    if (br.get(19) != kDcMarker) return false;
  } else {
    while (br.peek(10) == 1) br.skip(10);
    if (br.get(17) != kMotionMarker) return false;
  }
  // Partition 2.
  for (int k = 0; k < n; ++k) {
    PartitionMb& m = part[static_cast<uint64_t>(start + k)];
    if (m.skipped) {
      m.qscale = static_cast<uint8_t>(qscale_);
      continue;
    }
    if (m.intra) {
      m.ac_pred = br.bit() != 0;
      const int cbpy = read_vlc(br, lut_cbpy_, kCbpyLen);
      if (cbpy < 0) return false;
      m.cbp = static_cast<uint8_t>(m.cbpc | (cbpy << 2));
      if (!intra_plane) {
        m.use_dc_vlc = qscale_ < pic_.dc_threshold;
        if (m.dquant) qscale_ = clamp_int(qscale_ + kDquant[br.get(2)], 1, 31);
        m.qscale = static_cast<uint8_t>(qscale_);
        if (m.use_dc_vlc && !partition_dc(br, m)) return false;
      }
    } else {
      const int cbpy = read_vlc(br, lut_cbpy_, kCbpyLen);
      if (cbpy < 0) return false;
      m.cbp = static_cast<uint8_t>(m.cbpc | ((cbpy ^ 15) << 2));
      if (m.dquant) qscale_ = clamp_int(qscale_ + kDquant[br.get(2)], 1, 31);
      m.qscale = static_cast<uint8_t>(qscale_);
    }
    if (br.error()) return false;
  }
  // Partition 3: the texture, macroblock by macroblock.
  for (int k = 0; k < n; ++k) {
    const int index = start + k;
    const PartitionMb& m = part[static_cast<uint64_t>(index)];
    mb_x_ = index % mb_w_;
    mb_y_ = index / mb_w_;
    Mb& mb = mb_;
    clear_macroblock(mb);
    qscale_ = m.qscale;
    mb.intra = m.intra;
    mb.skipped = m.skipped;
    mb.ac_pred = m.ac_pred;
    mb.cbp = m.cbp;
    mb.mcsel = m.mcsel;
    if (!m.intra && (!m.skipped || m.mcsel)) {
      const MbInfo& info = cur_->mbs[static_cast<uint64_t>(index)];
      mb.mv_type = m.four ? kMv8x8 : kMv16x16;
      for (int i = 0; i < 4; ++i) {
        mb.mv[0][i][0] = info.mv[i][0];
        mb.mv[0][i][1] = info.mv[i][1];
      }
    }
    if (!m.skipped) {
      dc_given_ = m.use_dc_vlc;
      for (int i = 0; i < 6; ++i) given_dc_[i] = m.dc[i];
      rvlc_ = vol_.reversible_vlc;
      bool ok = true;
      for (int i = 0; i < 6 && ok; ++i) ok = block_mpeg4(br, mb, i, (mb.cbp >> (5 - i)) & 1, mb.intra, m.use_dc_vlc);
      dc_given_ = rvlc_ = false;
      if (!ok || br.error()) {
        *count = k;   // (the rest of the packet is concealed by the caller)
        return false;
      }
    }
    mb_qscale_[static_cast<uint64_t>(index)] = static_cast<uint8_t>(qscale_);
    record_macroblock(mb);
    reconstruct(mb);
  }
  *count = n;
  return true;
}

// The six DC differentials of an intra macroblock in the first or second partition (dct_dc_size and dct_dc_differential, as in a block).
bool Core::partition_dc(BitReader& br, PartitionMb& m) {
  for (int n = 0; n < 6; ++n) {
    const int size = n < 4 ? read_vlc(br, lut_dc_[0], kDcLumaLen) : read_vlc(br, lut_dc_[1], kDcChromaLen);
    if (size < 0) return false;
    int level = 0;
    if (size > 0) {
      const uint32_t v = br.get(size);
      level = (v >> (size - 1)) ? static_cast<int>(v) : static_cast<int>(v) - (1 << size) + 1;
      if (size > 8) br.skip(1);
    }
    m.dc[n] = static_cast<int16_t>(level);
  }
  return true;
}

// 7.8.4 / FFmpeg's mpeg4_decode_sprite_trajectory: the warp of an S plane from its trajectory - an offset and a gradient per component, in 1/2^shift samples
// (a single point: a translation in 1/(2 << accuracy) samples). False when the numbers do not fit the arithmetic of the prediction.
bool Core::setup_sprite(const VopHeader& h) {
  const int acc = vol_.warping_accuracy;
  const int64_t a = 2 << acc, rho = 3 - acc, r = 16 / a;
  const int64_t w = width_, hh = height_;
  int alpha = 1, beta = 0;
  while ((1 << alpha) < w) ++alpha;
  while ((1 << beta) < hh) ++beta;
  const int64_t w2 = int64_t{1} << alpha, h2 = int64_t{1} << beta;
  int64_t d[3][2] = {};
  for (int i = 0; i < vol_.warping_points && i < 3; ++i) {
    d[i][0] = h.sprite_trajectory[i][0];
    d[i][1] = h.sprite_trajectory[i][1];
  }
  const int64_t vop_ref[3][2] = {{0, 0}, {w, 0}, {0, hh}};
  int64_t sprite_ref[3][2];
  for (int c = 0; c < 2; ++c) {
    sprite_ref[0][c] = (a >> 1) * (2 * vop_ref[0][c] + d[0][c]);
    sprite_ref[1][c] = (a >> 1) * (2 * vop_ref[1][c] + d[0][c] + d[1][c]);
    sprite_ref[2][c] = (a >> 1) * (2 * vop_ref[2][c] + d[0][c] + d[2][c]);
  }
  auto rdiv = [](int64_t x, int64_t y) { return (x >= 0 ? x + (y >> 1) : x - (y >> 1)) / y; };
  int64_t virtual_ref[2][2];
  virtual_ref[0][0] = 16 * (vop_ref[0][0] + w2) + rdiv((w - w2) * (r * sprite_ref[0][0] - 16 * vop_ref[0][0]) + w2 * (r * sprite_ref[1][0] - 16 * vop_ref[1][0]), w);
  virtual_ref[0][1] = 16 * vop_ref[0][1] + rdiv((w - w2) * (r * sprite_ref[0][1] - 16 * vop_ref[0][1]) + w2 * (r * sprite_ref[1][1] - 16 * vop_ref[1][1]), w);
  virtual_ref[1][0] = 16 * vop_ref[0][0] + rdiv((hh - h2) * (r * sprite_ref[0][0] - 16 * vop_ref[0][0]) + h2 * (r * sprite_ref[2][0] - 16 * vop_ref[2][0]), hh);
  virtual_ref[1][1] = 16 * (vop_ref[0][1] + h2) + rdiv((hh - h2) * (r * sprite_ref[0][1] - 16 * vop_ref[0][1]) + h2 * (r * sprite_ref[2][1] - 16 * vop_ref[2][1]), hh);
  int64_t offset[2][2] = {}, delta[2][2] = {};
  int shift[2] = {0, 0};
  switch (vol_.warping_points) {
    case 0:
      delta[0][0] = delta[1][1] = a;
      break;
    case 1:
      offset[0][0] = sprite_ref[0][0] - a * vop_ref[0][0];
      offset[0][1] = sprite_ref[0][1] - a * vop_ref[0][1];
      offset[1][0] = ((sprite_ref[0][0] >> 1) | (sprite_ref[0][0] & 1)) - a * (vop_ref[0][0] / 2);
      offset[1][1] = ((sprite_ref[0][1] >> 1) | (sprite_ref[0][1] & 1)) - a * (vop_ref[0][1] / 2);
      delta[0][0] = delta[1][1] = a;
      break;
    case 2:
      offset[0][0] = sprite_ref[0][0] * (int64_t{1} << (alpha + rho)) + (-r * sprite_ref[0][0] + virtual_ref[0][0]) * (-vop_ref[0][0]) +
                     (r * sprite_ref[0][1] - virtual_ref[0][1]) * (-vop_ref[0][1]) + (int64_t{1} << (alpha + rho - 1));
      offset[0][1] = sprite_ref[0][1] * (int64_t{1} << (alpha + rho)) + (-r * sprite_ref[0][1] + virtual_ref[0][1]) * (-vop_ref[0][0]) +
                     (-r * sprite_ref[0][0] + virtual_ref[0][0]) * (-vop_ref[0][1]) + (int64_t{1} << (alpha + rho - 1));
      offset[1][0] = (-r * sprite_ref[0][0] + virtual_ref[0][0]) * (-2 * vop_ref[0][0] + 1) + (r * sprite_ref[0][1] - virtual_ref[0][1]) * (-2 * vop_ref[0][1] + 1) +
                     2 * w2 * r * sprite_ref[0][0] - 16 * w2 + (int64_t{1} << (alpha + rho + 1));
      offset[1][1] = (-r * sprite_ref[0][1] + virtual_ref[0][1]) * (-2 * vop_ref[0][0] + 1) + (-r * sprite_ref[0][0] + virtual_ref[0][0]) * (-2 * vop_ref[0][1] + 1) +
                     2 * w2 * r * sprite_ref[0][1] - 16 * w2 + (int64_t{1} << (alpha + rho + 1));
      delta[0][0] = -r * sprite_ref[0][0] + virtual_ref[0][0];
      delta[0][1] = r * sprite_ref[0][1] - virtual_ref[0][1];
      delta[1][0] = -r * sprite_ref[0][1] + virtual_ref[0][1];
      delta[1][1] = -r * sprite_ref[0][0] + virtual_ref[0][0];
      shift[0] = alpha + static_cast<int>(rho);
      shift[1] = alpha + static_cast<int>(rho) + 2;
      break;
    default: {
      const int min_ab = alpha < beta ? alpha : beta;
      const int64_t w3 = w2 >> min_ab, h3 = h2 >> min_ab;
      const int base = alpha + beta + static_cast<int>(rho) - min_ab;
      for (int c = 0; c < 2; ++c) {
        offset[0][c] = sprite_ref[0][c] * (int64_t{1} << base) + (-r * sprite_ref[0][c] + virtual_ref[0][c]) * h3 * (-vop_ref[0][0]) +
                       (-r * sprite_ref[0][c] + virtual_ref[1][c]) * w3 * (-vop_ref[0][1]) + (int64_t{1} << (base - 1));
        offset[1][c] = (-r * sprite_ref[0][c] + virtual_ref[0][c]) * h3 * (-2 * vop_ref[0][0] + 1) +
                       (-r * sprite_ref[0][c] + virtual_ref[1][c]) * w3 * (-2 * vop_ref[0][1] + 1) + 2 * w2 * h3 * r * sprite_ref[0][c] - 16 * w2 * h3 +
                       (int64_t{1} << (base + 1));
      }
      delta[0][0] = (-r * sprite_ref[0][0] + virtual_ref[0][0]) * h3;
      delta[0][1] = (-r * sprite_ref[0][0] + virtual_ref[1][0]) * w3;
      delta[1][0] = (-r * sprite_ref[0][1] + virtual_ref[0][1]) * h3;
      delta[1][1] = (-r * sprite_ref[0][1] + virtual_ref[1][1]) * w3;
      shift[0] = base;
      shift[1] = base + 2;
      break;
    }
  }
  int points = vol_.warping_points;
  if (delta[0][0] == (a << shift[0]) && delta[0][1] == 0 && delta[1][0] == 0 && delta[1][1] == (a << shift[0])) {
    // A translation after all.
    offset[0][0] >>= shift[0];
    offset[0][1] >>= shift[0];
    offset[1][0] >>= shift[1];
    offset[1][1] >>= shift[1];
    delta[0][0] = delta[1][1] = a;
    delta[0][1] = delta[1][0] = 0;
    shift[0] = shift[1] = 0;
    points = 1;
  } else {
    const int shift_y = 16 - shift[0], shift_c = 16 - shift[1];
    constexpr int64_t kMax = 0x7FFFFFFF;
    auto mag = [](int64_t v) { return v < 0 ? -v : v; };
    for (int i = 0; i < 2; ++i) {
      if (shift_c < 0 || shift_y < 0 || mag(offset[0][i]) >= (kMax >> shift_y) || mag(offset[1][i]) >= (kMax >> shift_c) || mag(delta[0][i]) >= (kMax >> shift_y) ||
          mag(delta[1][i]) >= (kMax >> shift_y))
        return false;
    }
    for (int i = 0; i < 2; ++i) {
      offset[0][i] *= int64_t{1} << shift_y;
      offset[1][i] *= int64_t{1} << shift_c;
      delta[0][i] *= int64_t{1} << shift_y;
      delta[1][i] *= int64_t{1} << shift_y;
      shift[i] = 16;
    }
    for (int i = 0; i < 2; ++i) {
      const int64_t sd0 = delta[i][0] - a * 65536, sd1 = delta[i][1] - a * 65536;
      if (mag(offset[0][i] + delta[i][0] * (w + 16)) >= kMax || mag(offset[0][i] + delta[i][1] * (hh + 16)) >= kMax ||
          mag(offset[0][i] + delta[i][0] * (w + 16) + delta[i][1] * (hh + 16)) >= kMax || mag(delta[i][0] * (w + 16)) >= kMax || mag(delta[i][1] * (hh + 16)) >= kMax ||
          mag(sd0) >= kMax || mag(sd1) >= kMax || mag(offset[0][i] + sd0 * (w + 16)) >= kMax || mag(offset[0][i] + sd1 * (hh + 16)) >= kMax ||
          mag(offset[0][i] + sd0 * (w + 16) + sd1 * (hh + 16)) >= kMax)
        return false;
    }
  }
  pic_.gmc = true;
  pic_.warping_points = points;
  pic_.accuracy = acc;
  for (int i = 0; i < 2; ++i)
    for (int j = 0; j < 2; ++j) {
      pic_.sprite_offset[i][j] = static_cast<int>(offset[i][j]);
      pic_.sprite_delta[i][j] = static_cast<int>(delta[i][j]);
    }
  pic_.sprite_shift[0] = shift[0];
  pic_.sprite_shift[1] = shift[1];
  return true;
}

// FFmpeg's get_amv: the vector a macroblock of an S plane predicted with the global motion stands for (for the prediction of its neighbours' vectors and for
// direct mode), the warp's mean over the macroblock, in the layer's sample unit, clipped to the f_code range.
int Core::global_vector(int n) const {
  const int a = pic_.accuracy, qpel = vol_.quarter_sample ? 1 : 0;
  const int64_t len = int64_t{1} << (pic_.fcode_forward + 4);
  auto rshift = [](int64_t v, int b) { return v > 0 ? (v + ((int64_t{1} << b) >> 1)) >> b : (v + ((int64_t{1} << b) >> 1) - 1) >> b; };
  int64_t sum;
  if (pic_.warping_points == 1) {
    sum = rshift(static_cast<int64_t>(pic_.sprite_offset[0][n]) * (1 << qpel), a);
  } else {
    int64_t dx = pic_.sprite_delta[n][0], dy = pic_.sprite_delta[n][1];
    const int shift = pic_.sprite_shift[0];
    if (n) dy -= int64_t{1} << (shift + a + 1);
    else dx -= int64_t{1} << (shift + a + 1);
    // (FFmpeg sums in 32-bit unsigned arithmetic; the checks of setup_sprite keep the terms in range.)
    const int32_t mb_v = static_cast<int32_t>(static_cast<uint32_t>(pic_.sprite_offset[0][n]) + static_cast<uint32_t>(dx) * static_cast<uint32_t>(mb_x_ * 16) +
                                              static_cast<uint32_t>(dy) * static_cast<uint32_t>(mb_y_ * 16));
    int64_t total = 0;
    for (int y = 0; y < 16; ++y) {
      int32_t v = static_cast<int32_t>(static_cast<uint32_t>(mb_v) + static_cast<uint32_t>(dy) * static_cast<uint32_t>(y));
      for (int x = 0; x < 16; ++x) {
        total += v >> shift;
        v = static_cast<int32_t>(static_cast<uint32_t>(v) + static_cast<uint32_t>(dx));
      }
    }
    sum = rshift(static_cast<int32_t>(total), a + 8 - qpel);
  }
  if (sum < -len) sum = -len;
  else if (sum >= len) sum = len - 1;
  return static_cast<int>(sum);
}

// What a later plane needs of this macroblock: its vectors for prediction (this plane) and for direct mode (a B plane after it).
void Core::record_macroblock(const Mb& mb) {
  if (pic_.type == kPictureB) return;
  MbInfo& info = cur_->mbs[static_cast<uint64_t>(mb_y_ * mb_w_ + mb_x_)];
  info.flags = 0;
  info.field_select[0] = info.field_select[1] = 0;
  if (mb.intra || (mb.skipped && !mb.mcsel)) {
    info.flags = mb.intra ? kMbIntra : kMbSkipped;
    // (In a PB-frame an intra macroblock's vector, used by its B part, predicts its neighbours' too: 6.1.1 rule 1.)
    const bool keep = mb.intra && pic_.short_header && pb_mode_;
    for (auto& v : info.mv) {
      v[0] = static_cast<int16_t>(keep ? mb.mv[0][0][0] : 0);
      v[1] = static_cast<int16_t>(keep ? mb.mv[0][0][1] : 0);
    }
    return;
  }
  if (mb.mv_type == kMv8x8) {
    info.flags = kMb8x8;
    for (int i = 0; i < 4; ++i) {
      info.mv[i][0] = static_cast<int16_t>(mb.mv[0][i][0]);
      info.mv[i][1] = static_cast<int16_t>(mb.mv[0][i][1]);
    }
    return;
  }
  int x = mb.mv[0][0][0], y = mb.mv[0][0][1];
  if (mb.mv_type == kMvField) {
    info.flags = kMbInterlaced;
    x = mb.mv[0][0][0] + mb.mv[0][1][0];
    x = (x >> 1) | (x & 1);
    y = mb.mv[0][0][1] + mb.mv[0][1][1];
    for (int i = 0; i < 2; ++i) {
      info.field_select[i] = static_cast<uint8_t>(mb.field_select[0][i]);
      info.field_mv[i][0] = static_cast<int16_t>(mb.mv[0][i][0]);
      info.field_mv[i][1] = static_cast<int16_t>(mb.mv[0][i][1]);
    }
  }
  for (auto& v : info.mv) {
    v[0] = static_cast<int16_t>(x);
    v[1] = static_cast<int16_t>(y);
  }
}

// ---- predictions ---------------------------------------------------------------------------------------------------------------------------

// Whether a macroblock is in the plane and in the video packet being decoded (decoded already): a neighbour that can be predicted from.
bool Core::same_packet(int mb_x, int mb_y) const {
  if (mb_x < 0 || mb_y < 0 || mb_x >= mb_w_ || mb_y >= mb_h_) return false;
  return mb_packet_[static_cast<uint64_t>(mb_y * mb_w_ + mb_x)] == packet_start_;
}

// 7.6.5: the motion vector predictor of a block of a P plane (block 0 for a whole macroblock), the median of the vectors to the left, above and above right, those
// outside the plane or the video packet left out.
void Core::predict_motion(int block, int* px, int* py) const {
  // [block][candidate]: macroblock offset (x, y) and block of the candidate.
  static const int8_t kCandidates[4][3][3] = {
      {{-1, 0, 1}, {0, -1, 2}, {1, -1, 2}}, {{0, 0, 0}, {0, -1, 3}, {1, -1, 2}}, {{-1, 0, 3}, {0, 0, 0}, {0, 0, 1}}, {{0, 0, 2}, {0, 0, 1}, {0, 0, 0}}};
  int v[3][2] = {};
  bool valid[3] = {};
  int count = 0;
  for (int c = 0; c < 3; ++c) {
    const int x = mb_x_ + kCandidates[block][c][0], y = mb_y_ + kCandidates[block][c][1], b = kCandidates[block][c][2];
    const bool inside = (x == mb_x_ && y == mb_y_) || same_packet(x, y);
    if (!inside) continue;
    const MbInfo& info = cur_->mbs[static_cast<uint64_t>(y * mb_w_ + x)];
    v[c][0] = info.mv[b][0];
    v[c][1] = info.mv[b][1];
    valid[c] = true;
    ++count;
  }
  if (count == 0) {
    *px = *py = 0;
    return;
  }
  if (count == 1) {
    for (int c = 0; c < 3; ++c)
      if (valid[c]) {
        *px = v[c][0];
        *py = v[c][1];
      }
    return;
  }
  auto median = [](int a, int b, int c) { return a > b ? (b > c ? b : a > c ? c : a) : (a > c ? a : b > c ? c : b); };
  *px = median(v[0][0], v[1][0], v[2][0]);
  *py = median(v[0][1], v[1][1], v[2][1]);
}

// 6.3.6.3 / H.263 5.3.7: a motion vector component from its difference and predictor, wrapped into the range of the f_code.
bool Core::read_motion(BitReader& br, int prediction, int fcode, int* value) {
  const int code = read_vlc(br, lut_mv_, kMvLen);
  if (code < 0) return false;
  if (code == 0) {
    *value = prediction;
    return true;
  }
  const bool negative = br.bit() != 0;
  const int shift = fcode - 1;
  int v = code;
  if (shift) {
    v = ((v - 1) << shift) | static_cast<int>(br.get(shift));
    ++v;
  }
  if (negative) v = -v;
  *value = sign_extend(static_cast<uint32_t>(v + prediction), 5 + fcode);
  return true;
}

// 7.4.3.1: the DC predictor of intra block `n` (0..3 luma, 4 and 5 chroma) and its direction (0: from the left, 1: from above); the reconstructed DC is kept
// for the blocks after it. A neighbour that is not an intra block of the same video packet counts as 1024.
int Core::predict_dc(int n, int level, int* direction) {
  const int scale = pic_.short_header ? 8 : n < 4 ? luma_dc_scale(qscale_) : chroma_dc_scale(qscale_);
  const int comp = n < 4 ? 0 : n - 3;
  const int stride = comp == 0 ? mb_w_ * 2 : mb_w_;
  const int bx = comp == 0 ? mb_x_ * 2 + (n & 1) : mb_x_, by = comp == 0 ? mb_y_ * 2 + (n >> 1) : mb_y_;
  ArrayView<int16_t> dc(dc_[comp].data(), dc_[comp].size());
  auto value = [&](int x, int y) -> int {
    const int mx = comp == 0 ? x >> 1 : x, my = comp == 0 ? y >> 1 : y;
    if (x < 0 || y < 0) return 1024;
    const bool own = mx == mb_x_ && my == mb_y_;
    if (!own && (!same_packet(mx, my) || !mb_intra_[static_cast<uint64_t>(my * mb_w_ + mx)])) return 1024;
    return dc[static_cast<uint64_t>(y * stride + x)];
  };
  const int a = value(bx - 1, by), b = value(bx - 1, by - 1), c = value(bx, by - 1);
  int pred;
  if ((a - b < 0 ? b - a : a - b) < (b - c < 0 ? c - b : b - c)) {
    pred = c;
    *direction = 1;
  } else {
    pred = a;
    *direction = 0;
  }
  pred = (pred + (scale >> 1)) / scale;
  level += pred;
  int stored = level * scale;
  if (stored & ~2047) stored = stored < 0 ? 0 : 2047;
  dc[static_cast<uint64_t>(by * stride + bx)] = static_cast<int16_t>(stored);
  return level;
}

// 7.4.3.3: AC prediction of the first row or column of intra block `n` from the block above or to the left (scaled to this macroblock's quantiser), and the
// copy of this block's first row and column for the blocks after it.
void Core::predict_ac(Mb& mb, int n, int direction) {
  const int comp = n < 4 ? 0 : n - 3;
  const int stride = comp == 0 ? mb_w_ * 2 : mb_w_;
  const int bx = comp == 0 ? mb_x_ * 2 + (n & 1) : mb_x_, by = comp == 0 ? mb_y_ * 2 + (n >> 1) : mb_y_;
  ArrayView<int16_t> ac(ac_[comp].data(), ac_[comp].size());
  ArrayView<int16_t> block(mb.block[n]);
  if (mb.ac_pred) {
    const int x = direction == 0 ? bx - 1 : bx, y = direction == 0 ? by : by - 1;
    if (x >= 0 && y >= 0) {
      const int mx = comp == 0 ? x >> 1 : x, my = comp == 0 ? y >> 1 : y;
      const bool own = mx == mb_x_ && my == mb_y_;
      const uint64_t neighbour_mb = static_cast<uint64_t>(my * mb_w_ + mx);
      if (own || (same_packet(mx, my) && mb_intra_[neighbour_mb])) {
        const int q = own ? qscale_ : mb_qscale_[neighbour_mb];
        const ArrayView<int16_t> from = ac.subview(static_cast<uint64_t>(y * stride + x) * 16, 16);
        for (int i = 1; i < 8; ++i) {
          const int v = direction == 0 ? from[static_cast<uint64_t>(i)] : from[static_cast<uint64_t>(8 + i)];
          const int add = q == qscale_ ? v : rounded_div(v * q, qscale_);
          const uint64_t at = direction == 0 ? static_cast<uint64_t>(i * 8) : static_cast<uint64_t>(i);
          block[at] = static_cast<int16_t>(block[at] + add);
        }
      }
    }
  }
  const ArrayView<int16_t> to = ac.subview(static_cast<uint64_t>(by * stride + bx) * 16, 16);
  for (int i = 1; i < 8; ++i) {
    to[static_cast<uint64_t>(i)] = block[static_cast<uint64_t>(i * 8)];
    to[static_cast<uint64_t>(8 + i)] = block[static_cast<uint64_t>(i)];
  }
}

// 7.6.9.5: the vectors of a direct-mode macroblock of a B plane from the colocated macroblock of the backward reference, scaled by the temporal distances, plus
// the delta (mx, my). Rounded and laid out as FFmpeg's ff_mpeg4_set_direct_mv does.
void Core::set_direct(Mb& mb, int mx, int my) {
  const MbInfo& col = anchor_[1]->mbs[static_cast<uint64_t>(mb_y_ * mb_w_ + mb_x_)];
  mb.dir = kForward | kBackward;
  if (col.flags & kMbInterlaced) {
    mb.mv_type = kMvField;
    for (int i = 0; i < 2; ++i) {
      const int field_select = col.field_select[i];
      mb.field_select[0][i] = field_select;
      mb.field_select[1][i] = i;
      int pp, pb;
      if (pic_.top_field_first) {
        pp = static_cast<uint16_t>(pp_field_time_ - field_select + i);
        pb = static_cast<uint16_t>(pb_field_time_ - field_select + i);
      } else {
        pp = static_cast<uint16_t>(pp_field_time_ + field_select - i);
        pb = static_cast<uint16_t>(pb_field_time_ + field_select - i);
      }
      if (pp == 0) pp = 1;
      const int px = col.field_mv[i][0], py = col.field_mv[i][1];
      mb.mv[0][i][0] = px * pb / pp + mx;
      mb.mv[0][i][1] = py * pb / pp + my;
      mb.mv[1][i][0] = mx ? mb.mv[0][i][0] - px : px * (pb - pp) / pp;
      mb.mv[1][i][1] = my ? mb.mv[0][i][1] - py : py * (pb - pp) / pp;
    }
    return;
  }
  int pp = static_cast<uint16_t>(pp_time_), pb = static_cast<uint16_t>(pb_time_);
  if (pp == 0) pp = 1;
  const int blocks = (col.flags & kMb8x8) ? 4 : 1;
  for (int i = 0; i < blocks; ++i) {
    const int px = col.mv[i][0], py = col.mv[i][1];
    mb.mv[0][i][0] = px * pb / pp + mx;
    mb.mv[0][i][1] = py * pb / pp + my;
    mb.mv[1][i][0] = mx ? mb.mv[0][i][0] - px : px * (pb - pp) / pp;
    mb.mv[1][i][1] = my ? mb.mv[0][i][1] - py : py * (pb - pp) / pp;
  }
  if (blocks == 1) {
    for (int d = 0; d < 2; ++d)
      for (int i = 1; i < 4; ++i) {
        mb.mv[d][i][0] = mb.mv[d][0][0];
        mb.mv[d][i][1] = mb.mv[d][0][1];
      }
  }
  // With quarter-sample motion FFmpeg (and the encoders made against it) predicts a 16x16 direct macroblock as four 8x8 blocks.
  mb.mv_type = (blocks == 4 || vol_.quarter_sample) ? kMv8x8 : kMv16x16;
}

// ---- macroblocks --------------------------------------------------------------------------------------------------------------------------


// 6.2.6 macroblock() for I, P and B planes (no data partitioning, no sprites).
bool Core::macroblock_mpeg4(BitReader& br, Mb& mb) {
  clear_macroblock(mb);
  const uint64_t index = static_cast<uint64_t>(mb_y_ * mb_w_ + mb_x_);
  int cbpc = 0;
  if (pic_.type == kPictureB) {
    // A macroblock that was not coded in the backward reference is not coded here either: a copy of the forward reference.
    if (anchor_[1]->mbs[index].flags & kMbSkipped) {
      mb.skipped = true;
      return true;
    }
    bool direct = false;
    int cbp = 0;
    const bool modb1 = br.bit() != 0;
    bool field = false;
    if (modb1) {
      direct = true;
    } else {
      const bool modb2 = br.bit() != 0;
      const int type = read_vlc(br, lut_btype_, kBTypeLen);
      if (type < 0) return false;
      direct = type == 0;
      mb.dir = type == 1 ? (kForward | kBackward) : type == 2 ? kBackward : type == 3 ? kForward : (kForward | kBackward);
      if (!modb2) cbp = static_cast<int>(br.get(6));
      if (!direct && cbp && br.bit()) qscale_ = clamp_int(qscale_ + (br.bit() ? 2 : -2), 1, 31);   // dbquant
      if (vol_.interlaced) {
        if (cbp) mb.field_dct = br.bit() != 0;
        if (!direct && br.bit()) {
          field = true;
          if (mb.dir & kForward) {
            mb.field_select[0][0] = br.bit();
            mb.field_select[0][1] = br.bit();
          }
          if (mb.dir & kBackward) {
            mb.field_select[1][0] = br.bit();
            mb.field_select[1][1] = br.bit();
          }
        }
      }
      if (!direct) {
        const int fcode[2] = {pic_.fcode_forward, pic_.fcode_backward};
        mb.mv_type = field ? kMvField : kMv16x16;
        for (int d = 0; d < 2; ++d) {
          if (!(mb.dir & (1 << d))) continue;
          if (!field) {
            int mx, my;
            if (!read_motion(br, last_mv_[d][0][0], fcode[d], &mx) || !read_motion(br, last_mv_[d][0][1], fcode[d], &my)) return false;
            mb.mv[d][0][0] = mx;
            mb.mv[d][0][1] = my;
            last_mv_[d][0][0] = last_mv_[d][1][0] = mx;
            last_mv_[d][0][1] = last_mv_[d][1][1] = my;
          } else {
            for (int i = 0; i < 2; ++i) {
              int mx, my;
              if (!read_motion(br, last_mv_[d][i][0], fcode[d], &mx) || !read_motion(br, last_mv_[d][i][1] / 2, fcode[d], &my)) return false;
              mb.mv[d][i][0] = mx;
              mb.mv[d][i][1] = my;
              last_mv_[d][i][0] = mx;
              last_mv_[d][i][1] = my * 2;
            }
          }
        }
      }
    }
    if (direct) {
      int mx = 0, my = 0;
      if (!modb1 && (!read_motion(br, 0, 1, &mx) || !read_motion(br, 0, 1, &my))) return false;
      set_direct(mb, mx, my);
    }
    mb.cbp = cbp;
    for (int i = 0; i < 6; ++i)
      if (!block_mpeg4(br, mb, i, (cbp >> (5 - i)) & 1, false, false)) return false;
    return true;
  }

  if (pic_.type == kPictureP) {
    for (;;) {
      if (br.bit()) {   // not_coded
        mb.skipped = true;
        if (pic_.gmc) {   // (in an S plane: the global motion, no residual)
          mb.mcsel = true;
          mb.mv[0][0][0] = global_vector(0);
          mb.mv[0][0][1] = global_vector(1);
        }
        return true;
      }
      cbpc = read_vlc(br, lut_inter_mcbpc_, kMcbpcLen);
      if (cbpc < 0) return false;
      if (cbpc != kStuffingInter) break;
      if (br.error()) return false;
    }
    mb.intra = (cbpc & 4) != 0;
    if (!mb.intra) {
      if (pic_.gmc && (cbpc & 16) == 0) mb.mcsel = br.bit() != 0;
      const int cbpy = read_vlc(br, lut_cbpy_, kCbpyLen);
      if (cbpy < 0) return false;
      mb.cbp = (cbpc & 3) | ((cbpy ^ 15) << 2);
      if (cbpc & 8) qscale_ = clamp_int(qscale_ + kDquant[br.get(2)], 1, 31);
      if (vol_.interlaced && mb.cbp) mb.field_dct = br.bit() != 0;
      MbInfo& info = cur_->mbs[index];
      if (mb.mcsel) {
        mb.mv[0][0][0] = global_vector(0);
        mb.mv[0][0][1] = global_vector(1);
      } else if ((cbpc & 16) == 0) {
        int px, py;
        if (vol_.interlaced && br.bit()) {
          mb.mv_type = kMvField;
          mb.field_select[0][0] = br.bit();
          mb.field_select[0][1] = br.bit();
          predict_motion(0, &px, &py);
          for (int i = 0; i < 2; ++i) {
            if (!read_motion(br, px, pic_.fcode_forward, &mb.mv[0][i][0]) || !read_motion(br, py / 2, pic_.fcode_forward, &mb.mv[0][i][1])) return false;
          }
        } else {
          predict_motion(0, &px, &py);
          if (!read_motion(br, px, pic_.fcode_forward, &mb.mv[0][0][0]) || !read_motion(br, py, pic_.fcode_forward, &mb.mv[0][0][1])) return false;
        }
      } else {
        mb.mv_type = kMv8x8;
        for (int i = 0; i < 4; ++i) {
          int px, py;
          predict_motion(i, &px, &py);
          if (!read_motion(br, px, pic_.fcode_forward, &mb.mv[0][i][0]) || !read_motion(br, py, pic_.fcode_forward, &mb.mv[0][i][1])) return false;
          info.mv[i][0] = static_cast<int16_t>(mb.mv[0][i][0]);
          info.mv[i][1] = static_cast<int16_t>(mb.mv[0][i][1]);
        }
      }
      for (int i = 0; i < 6; ++i)
        if (!block_mpeg4(br, mb, i, (mb.cbp >> (5 - i)) & 1, false, false)) return false;
      return true;
    }
  } else {
    do {
      cbpc = read_vlc(br, lut_intra_mcbpc_, kMcbpcLen);
      if (cbpc < 0 || br.error()) return false;
    } while (cbpc == kStuffingIntra);
    mb.intra = true;
    cbpc = (cbpc & 3) | ((cbpc & 4) << 1);   // the dquant flag where an inter mcbpc has it
  }
  // An intra macroblock (of an I or a P plane).
  mb.ac_pred = br.bit() != 0;
  const int cbpy = read_vlc(br, lut_cbpy_, kCbpyLen);
  if (cbpy < 0) return false;
  mb.cbp = (cbpc & 3) | (cbpy << 2);
  const bool use_dc_vlc = qscale_ < pic_.dc_threshold;   // (the quantiser before this macroblock's dquant)
  if (cbpc & 8) qscale_ = clamp_int(qscale_ + kDquant[br.get(2)], 1, 31);
  if (vol_.interlaced) mb.field_dct = br.bit() != 0;
  for (int i = 0; i < 6; ++i)
    if (!block_mpeg4(br, mb, i, (mb.cbp >> (5 - i)) & 1, true, use_dc_vlc)) return false;
  return true;
}

// Annex T: the chroma quantiser (the luma one without modified quantisation).
int Core::chroma_quant(int q) const { return pic_.short_header && pic_.modified_quant ? kChromaQuant[q & 31] : q; }

// H.263 5.3.6 DQUANT, or Annex T.2's: a step through Table T.1, or the quantiser itself.
void Core::h263_dquant(BitReader& br) {
  if (pic_.modified_quant) {
    if (br.bit()) qscale_ = kModifiedQuant[br.bit()][qscale_ & 31];
    else qscale_ = static_cast<int>(br.get(5));
  } else {
    qscale_ += kDquant[br.get(2)];
  }
  qscale_ = clamp_int(qscale_, 1, 31);
}

// A motion vector component of H.263: the baseline code (wrapped to [-16, 15.5]), Annex D of version 1 (the range around the predictor) or of version 2 (UVLC).
bool Core::h263_motion(BitReader& br, int prediction, int* value) {
  if (pic_.umv_plus) {
    if (br.bit()) {
      *value = prediction;
      return true;
    }
    int code = 2 + br.bit();
    while (br.bit()) {
      code = (code << 1) + br.bit();
      if (code >= 32768 || br.error()) return false;
    }
    const bool negative = code & 1;
    code >>= 1;
    *value = negative ? prediction - code : prediction + code;
    return true;
  }
  if (!pic_.long_vectors) return read_motion(br, prediction, 1, value);
  const int code = read_vlc(br, lut_mv_, kMvLen);
  if (code < 0) return false;
  if (code == 0) {
    *value = prediction;
    return true;
  }
  int v = br.bit() ? -code : code;
  v += prediction;
  if (prediction < -31 && v < -63) v += 64;
  if (prediction > 32 && v > 63) v -= 64;
  *value = v;
  return true;
}

// The vectors of an inter macroblock (four with advanced prediction), predicted as in 6.1.1, with Annex D's stuffing bit after a (1, 1) difference.
bool Core::h263_vectors(BitReader& br, Mb& mb, bool four) {
  MbInfo& info = cur_->mbs[static_cast<uint64_t>(mb_y_ * mb_w_ + mb_x_)];
  for (int i = 0; i < (four ? 4 : 1); ++i) {
    int px, py, mx, my;
    predict_motion(i, &px, &py);
    if (!h263_motion(br, px, &mx) || !h263_motion(br, py, &my)) return false;
    if (pic_.umv_plus && mx - px == 1 && my - py == 1) br.skip(1);
    mb.mv[0][i][0] = mx;
    mb.mv[0][i][1] = my;
    info.mv[i][0] = static_cast<int16_t>(mx);
    info.mv[i][1] = static_cast<int16_t>(my);
  }
  mb.mv_type = four ? kMv8x8 : kMv16x16;
  return true;
}

// H.263 5.3: a macroblock of an I or a P picture, with the optional modes of the picture header.
bool Core::macroblock_h263(BitReader& br, Mb& mb) {
  clear_macroblock(mb);
  int cbpc = 0;
  if (pic_.type == kPictureP) {
    for (;;) {
      if (br.bit()) {   // COD
        mb.skipped = true;
        return true;
      }
      cbpc = read_vlc(br, lut_inter_mcbpc_, kMcbpcLen);
      if (cbpc < 0) return false;
      if (cbpc != kStuffingInter) break;
      if (br.error()) return false;
    }
    if ((cbpc & 16) && !pic_.obmc) return false;   // INTER4V needs advanced prediction (Annex F)
  } else {
    do {
      cbpc = read_vlc(br, lut_intra_mcbpc_, kMcbpcLen);
      if (cbpc < 0 || br.error()) return false;
    } while (cbpc == kStuffingIntra);
    cbpc = 4 | (cbpc & 3) | ((cbpc & 4) << 1);
  }
  mb.intra = (cbpc & 4) != 0;
  if (mb.intra && pic_.aic) {
    mb.ac_pred = br.bit() != 0;   // INTRA_MODE: 0, 10 (from the left) or 11 (from above)
    if (mb.ac_pred) mb.aic_left = br.bit() != 0;
  }
  const bool pb = pb_mode_ && pic_.type == kPictureP;
  bool has_mvdb = false;
  if (pb) {
    // 5.3.3 MODB: Table 11 (Annex G: 0, 10 MVDB, 11 CBPB and MVDB) or Table M.1 (0 and 10: bidirectional, 110 and 1110: forward with MVDB, 11110 and
    // 11111: backward; the second of each pair with CBPB).
    bool has_cbpb = false;
    if (pb_mode_ == 1) {
      if (br.bit()) {
        has_mvdb = true;
        has_cbpb = br.bit() != 0;
      }
    } else {
      int ones = 0;
      while (ones < 5 && br.bit()) ++ones;
      static const uint8_t kModes[6] = {kBBidirectional, kBBidirectional, kBForward, kBForward, kBBackward, kBBackward};
      mb.b_mode = kModes[ones];
      has_cbpb = ones == 1 || ones == 3 || ones == 5;
      has_mvdb = ones == 2 || ones == 3;
    }
    if (has_cbpb) mb.cbpb = static_cast<int>(br.get(6));
  }
  int cbpy = read_vlc(br, lut_cbpy_, kCbpyLen);
  if (cbpy < 0) return false;
  if (!mb.intra && (!pic_.aiv || (cbpc & 3) != 3)) cbpy ^= 15;   // (Annex S: not inverted when both chroma blocks are coded)
  mb.cbp = (cbpc & 3) | (cbpy << 2);
  if (cbpc & 8) h263_dquant(br);
  if (!mb.intra && !h263_vectors(br, mb, (cbpc & 16) != 0)) return false;
  // In a PB-frame an intra macroblock carries a vector for its B part too (Annex M: in bidirectional mode only).
  if (mb.intra && pb && (pb_mode_ == 1 || mb.b_mode == kBBidirectional)) {
    if (!h263_vectors(br, mb, false)) return false;
    mb.mv_type = kMv16x16;
  }
  if (has_mvdb) {
    if (pb_mode_ == 1) {
      // G.4: MVDB is coded against Pc = (TRB x MV) / TRD of the (first) P vector; what it adds to Pc is the delta MVD of every block.
      for (int c = 0; c < 2; ++c) {
        const int pc = trb_ * mb.mv[0][0][c] / trd_;
        int v;
        if (!h263_motion(br, pc, &v)) return false;
        mb.mvd[c] = v - pc;
      }
    } else {
      // M.2.2: the forward vector, predicted by the left macroblock's forward vector (or zero).
      int px = 0, py = 0;
      if (mb_x_ > 0 && same_packet(mb_x_ - 1, mb_y_)) {
        const BVectors& left = bvec_[static_cast<uint64_t>(mb_y_ * mb_w_ + mb_x_ - 1)];
        if (left.has_fw) {
          px = left.fw[0];
          py = left.fw[1];
        }
      }
      if (!h263_motion(br, px, &mb.mvd[0]) || !h263_motion(br, py, &mb.mvd[1])) return false;
      if (pic_.umv_plus && mb.mvd[0] - px == 1 && mb.mvd[1] - py == 1) br.skip(1);
    }
  }
  if (pb && pb_mode_ == 2 && mb.b_mode == kBForward) {
    BVectors& own = bvec_[static_cast<uint64_t>(mb_y_ * mb_w_ + mb_x_)];
    own.fw[0] = static_cast<int16_t>(mb.mvd[0]);
    own.fw[1] = static_cast<int16_t>(mb.mvd[1]);
    own.has_fw = 1;
  }
  for (int i = 0; i < 6; ++i)
    if (!block_h263(br, mb, i, (mb.cbp >> (5 - i)) & 1, mb.intra)) return false;
  if (pb)
    for (int i = 0; i < 6; ++i)
      if (!block_h263(br, mb, i, (mb.cbpb >> (5 - i)) & 1, false, true)) return false;
  return true;
}

// 5.1.23 Table 6: BQUANT, the quantiser of a PB-frame's B blocks.
int Core::b_quant() const {
  const int q = (5 + dbquant_) * qscale_ / 4;
  return q > 31 ? 31 : q < 1 ? 1 : q;
}

// O.5.1: a vector of a B picture's macroblock, predicted from the neighbours' vectors of the same direction (those without one count as zero) with the
// border rules of 6.1.1.
bool Core::h263_b_vector(BitReader& br, int dir, int* x, int* y) {
  static const int8_t kCandidates[3][2] = {{-1, 0}, {0, -1}, {1, -1}};
  int v[3][2] = {};
  bool valid[3] = {};
  int count = 0;
  for (int c = 0; c < 3; ++c) {
    const int nx = mb_x_ + kCandidates[c][0], ny = mb_y_ + kCandidates[c][1];
    if (!same_packet(nx, ny)) continue;
    const BVectors& n = bvec_[static_cast<uint64_t>(ny * mb_w_ + nx)];
    if (dir == 0 ? n.has_fw : n.has_bw) {
      v[c][0] = dir == 0 ? n.fw[0] : n.bw[0];
      v[c][1] = dir == 0 ? n.fw[1] : n.bw[1];
    }
    valid[c] = true;
    ++count;
  }
  int px = 0, py = 0;
  if (count == 1) {
    for (int c = 0; c < 3; ++c)
      if (valid[c]) {
        px = v[c][0];
        py = v[c][1];
      }
  } else if (count > 1) {
    auto median = [](int a, int b, int c) { return a > b ? (b > c ? b : a > c ? c : a) : (a > c ? a : b > c ? c : b); };
    px = median(v[0][0], v[1][0], v[2][0]);
    py = median(v[0][1], v[1][1], v[2][1]);
  }
  if (!h263_motion(br, px, x) || !h263_motion(br, py, y)) return false;
  if (pic_.umv_plus && *x - px == 1 && *y - py == 1) br.skip(1);
  BVectors& own = bvec_[static_cast<uint64_t>(mb_y_ * mb_w_ + mb_x_)];
  if (dir == 0) {
    own.fw[0] = static_cast<int16_t>(*x);
    own.fw[1] = static_cast<int16_t>(*y);
    own.has_fw = 1;
  } else {
    own.bw[0] = static_cast<int16_t>(*x);
    own.bw[1] = static_cast<int16_t>(*y);
    own.has_bw = 1;
  }
  return true;
}

// O.5.2 / G.4 with MVD = 0: the vectors of a direct-mode macroblock from the backward reference's colocated vectors (zero where it is intra).
void Core::h263_direct(Mb& mb) {
  const MbInfo& col = anchor_[1]->mbs[static_cast<uint64_t>(mb_y_ * mb_w_ + mb_x_)];
  const bool intra = (col.flags & kMbIntra) != 0;
  mb.dir = kForward | kBackward;
  mb.mv_type = (col.flags & kMb8x8) ? kMv8x8 : kMv16x16;
  for (int i = 0; i < 4; ++i)
    for (int c = 0; c < 2; ++c) {
      const int v = intra ? 0 : col.mv[i][c];
      mb.mv[0][i][c] = trb_ * v / trd_;
      mb.mv[1][i][c] = (trb_ - trd_) * v / trd_;
    }
}

// O.4: a macroblock of a B picture: COD, MBTYPE (Table O.1), [INTRA_MODE], CBPC (Table O.4) and CBPY, DQUANT, MVDFW, MVDBW.
bool Core::macroblock_h263_b(BitReader& br, Mb& mb) {
  clear_macroblock(mb);
  if (br.bit()) {   // COD: direct, nothing coded
    h263_direct(mb);
    mb.skipped = false;
    return true;
  }
  // Table O.1 as (code, length): direct, direct+Q, forward (no texture), forward, forward+Q, backward (no texture), backward, backward+Q, bi-dir (no texture),
  // bi-dir, bi-dir+Q, intra, intra+Q, stuffing.
  static const uint8_t kCode[14] = {3, 1, 4, 5, 6, 2, 3, 7, 4, 5, 1, 1, 1, 1};
  static const uint8_t kLength[14] = {2, 4, 3, 3, 5, 3, 3, 5, 5, 5, 5, 6, 7, 9};
  int type = -1;
  do {
    type = -1;
    for (int t = 0; t < 14 && type < 0; ++t)
      if (br.peek(kLength[t]) == kCode[t]) {
        br.skip(kLength[t]);
        type = t;
      }
    if (type < 0 || br.error()) return false;
  } while (type == 13);
  const bool coded = type == 0 || type == 1 || type == 3 || type == 4 || type == 6 || type == 7 || type == 9 || type == 10 || type == 11 || type == 12;
  const bool quant = type == 1 || type == 4 || type == 7 || type == 10 || type == 12;
  const bool forward = type >= 2 && type <= 4, backward = type >= 5 && type <= 7, bidirectional = type >= 8 && type <= 10;
  mb.intra = type >= 11;
  if (mb.intra && pic_.aic) {
    mb.ac_pred = br.bit() != 0;
    if (mb.ac_pred) mb.aic_left = br.bit() != 0;
  }
  if (coded) {
    int cbpc;
    if (!br.bit()) cbpc = 0;            // 0
    else if (!br.bit()) cbpc = 1;       // 10
    else cbpc = br.bit() ? 2 : 3;       // 111: 10, 110: 11
    int cbpy = read_vlc(br, lut_cbpy_, kCbpyLen);
    if (cbpy < 0) return false;
    if (!mb.intra && (!pic_.aiv || cbpc != 3)) cbpy ^= 15;
    mb.cbp = cbpc | (cbpy << 2);
  }
  if (quant) h263_dquant(br);
  if (mb.intra) {
    // (no vectors)
  } else if (type <= 1) {
    h263_direct(mb);
  } else {
    mb.mv_type = kMv16x16;
    mb.dir = 0;
    if (forward || bidirectional) {
      if (!h263_b_vector(br, 0, &mb.mv[0][0][0], &mb.mv[0][0][1])) return false;
      mb.dir |= kForward;
    }
    if (backward || bidirectional) {
      if (!h263_b_vector(br, 1, &mb.mv[1][0][0], &mb.mv[1][0][1])) return false;
      mb.dir |= kBackward;
    }
  }
  for (int i = 0; i < 6; ++i)
    if (!block_h263(br, mb, i, (mb.cbp >> (5 - i)) & 1, mb.intra)) return false;
  return true;
}

// Annex F needs the vectors of the macroblock to the right before this one is predicted: they are read ahead from a copy of the reader (FFmpeg's
// preview_obmc) and kept in the picture's MbInfo, where decoding that macroblock writes them again. A macroblock that cannot be read counts as intra.
void Core::preview_next(const BitReader& from) {
  BitReader br = from;
  const int saved_q = qscale_, saved_x = mb_x_, saved_packet = packet_start_;
  // (The encoder overlaps across group of blocks and slice boundaries too: a header in between is read past.)
  if (br.bits_left() >= 16 && br.peek(16) == 0) {
    BitReader at = br;
    int n = -1;
    if (gob_header(at, &n) && n == mb_y_ * mb_w_ + mb_x_ + 1) {
      br = at;
      packet_start_ = n;
    }
    qscale_ = saved_q;   // (gob_header set the slice's quantiser; only the vectors are wanted here)
  }
  ++mb_x_;
  MbInfo& info = cur_->mbs[static_cast<uint64_t>(mb_y_ * mb_w_ + mb_x_)];
  info.flags = kMbIntra;
  for (auto& v : info.mv) v[0] = v[1] = 0;
  int cbpc = -1;
  bool skipped = false;
  for (;;) {
    if (br.bit()) {
      skipped = true;
      break;
    }
    cbpc = read_vlc(br, lut_inter_mcbpc_, kMcbpcLen);
    if (cbpc != kStuffingInter || br.error()) break;
  }
  if (skipped) {
    info.flags = kMbSkipped;
  } else if (cbpc >= 0 && !(cbpc & 4)) {
    if (read_vlc(br, lut_cbpy_, kCbpyLen) >= 0) {
      if (cbpc & 8) h263_dquant(br);
      Mb next;
      if (h263_vectors(br, next, (cbpc & 16) != 0) && !br.error()) info.flags = (cbpc & 16) ? kMb8x8 : 0;
      else for (auto& v : info.mv) v[0] = v[1] = 0;
      if (!(cbpc & 16))
        for (auto& v : info.mv) {
          v[0] = static_cast<int16_t>(next.mv[0][0][0]);
          v[1] = static_cast<int16_t>(next.mv[0][0][1]);
        }
    }
  }
  qscale_ = saved_q;
  mb_x_ = saved_x;
  packet_start_ = saved_packet;
}

// ---- blocks --------------------------------------------------------------------------------------------------------------------------------

// 6.2.8 block(): the coefficients of block `n` of an MPEG-4 macroblock, in scan order into position order. Intra blocks get their DC and AC predictions here
// (their levels stay quantised: Mpeg4Recon.cpp dequantises them); the levels of inter blocks under the H.263 quantiser are dequantised as they are read, as
// FFmpeg does (which matters for the clipping of the third escape).
bool Core::block_mpeg4(BitReader& br, Mb& mb, int n, bool coded, bool intra, bool use_dc_vlc) {
  ArrayView<int16_t> block(mb.block[n]);
  int pos = 0, direction = 0, last_pos = -1;
  int dc_level = 0;
  if (intra) {
    if (use_dc_vlc && dc_given_) {
      dc_level = given_dc_[n];
      pos = 1;
      last_pos = 0;
    } else if (use_dc_vlc) {
      const int size = n < 4 ? read_vlc(br, lut_dc_[0], kDcLumaLen) : read_vlc(br, lut_dc_[1], kDcChromaLen);
      if (size < 0) return false;
      if (size > 0) {
        const uint32_t v = br.get(size);
        dc_level = (v >> (size - 1)) ? static_cast<int>(v) : static_cast<int>(v) - (1 << size) + 1;
        if (size > 8) br.skip(1);   // marker_bit
      }
      pos = 1;
      last_pos = 0;
    }
    // The direction of the prediction picks the scan; it does not depend on this block's DC. (This stores a provisional DC, replaced below.)
    predict_dc(n, 0, &direction);
  }
  if (coded) {
    const int table = intra ? 0 : 1;
    const CoefficientTable& t = intra ? kIntraCoefficients : kInterCoefficients;
    const int scan_index = pic_.alternate_scan ? 2 : (intra && mb.ac_pred) ? (direction == 0 ? 2 : 1) : 0;
    const ArrayView<const uint8_t> scan(scan_tables_[scan_index]);
    const bool h263_quant = !intra && !vol_.quant_type_mpeg;
    const int qmul = h263_quant ? qscale_ * 2 : 1, qadd = h263_quant ? (qscale_ - 1) | 1 : 0;
    for (;;) {
      int run, level, last;
      if (rvlc_) {
        // B.1.7: reversible codes (read forwards); the escape is fixed length and framed by its own escape code.
        const ReversibleTable& rt = intra ? kIntraReversible : kInterReversible;
        const int code = read_vlc(br, lut_rvlc_[table], kReversibleLen);
        if (code < 0) return false;
        if (code == 169) {
          if (!br.bit()) return false;   // marker_bit
          last = br.bit();
          run = static_cast<int>(br.get(6));
          if (!br.bit()) return false;   // marker_bit
          level = static_cast<int>(br.get(11));
          if (br.get(5) != 0x10 || level == 0) return false;   // marker_bit and the closing escape
        } else {
          last = code >= kReversibleLastStart ? 1 : 0;
          run = rt.run[code];
          level = rt.level[code];
        }
        level = level * qmul + qadd;
        if (br.bit()) level = -level;
        if (level > 2047) level = 2047;
        if (level < -2048) level = -2048;
        pos += run;
        if (pos > 63) return false;
        block[scan[static_cast<uint64_t>(pos)]] = static_cast<int16_t>(level);
        last_pos = pos;
        ++pos;
        if (last) break;
        if (pos > 63 || br.error()) return false;
        continue;
      }
      int sym = read_vlc(br, lut_coef_[table], kCoefficientLen);
      if (sym < 0) return false;
      if (sym == kEscape) {
        if (!br.bit()) {   // type 1: the level is beyond the table's for this run
          sym = read_vlc(br, lut_coef_[table], kCoefficientLen);
          if (sym < 0 || sym == kEscape) return false;
          last = sym >= t.last_start ? 1 : 0;
          run = t.run[sym];
          level = t.level[sym] + max_level_[table][last][run];
          if (br.bit()) level = -level;
          level = level > 0 ? level * qmul + qadd : level * qmul - qadd;
        } else if (!br.bit()) {   // type 2: the run is beyond the table's for this level
          sym = read_vlc(br, lut_coef_[table], kCoefficientLen);
          if (sym < 0 || sym == kEscape) return false;
          last = sym >= t.last_start ? 1 : 0;
          level = t.level[sym];
          run = t.run[sym] + max_run_[table][last][level] + 1;
          if (br.bit()) level = -level;
          level = level > 0 ? level * qmul + qadd : level * qmul - qadd;
        } else {   // type 3: fixed length
          last = br.bit();
          run = static_cast<int>(br.get(6));
          if (!br.bit()) return false;   // marker_bit
          level = sign_extend(br.get(12), 12);
          if (!br.bit()) return false;   // marker_bit
          if (level == 0) return false;
          level = level > 0 ? level * qmul + qadd : level * qmul - qadd;
          if (level > 2047) level = 2047;
          if (level < -2048) level = -2048;
        }
      } else {
        last = sym >= t.last_start ? 1 : 0;
        run = t.run[sym];
        level = t.level[sym];
        if (br.bit()) level = -level;
        level = level > 0 ? level * qmul + qadd : level * qmul - qadd;
      }
      pos += run;
      if (pos > 63) return false;
      block[scan[static_cast<uint64_t>(pos)]] = static_cast<int16_t>(level);
      last_pos = pos;
      ++pos;
      if (last) break;
      if (pos > 63 || br.error()) return false;
    }
  }
  if (intra) {
    const int level = use_dc_vlc ? dc_level : block[0];
    int dir = 0;
    block[0] = static_cast<int16_t>(predict_dc(n, level, &dir));
    predict_ac(mb, n, direction);
    if (last_pos < 0) last_pos = 0;
    if (mb.ac_pred) last_pos = 63;
  }
  mb.last[n] = last_pos;
  return true;
}

// H.263 5.4: a block: INTRADC (intra blocks), then the coefficients with the H.263 table; escapes are fixed length (LAST, RUN, an 8-bit LEVEL, extended
// by Annex T when it is -128). Under advanced intra coding (Annex I) an intra block's DC is among its coefficients, coded with Table I.2 and predicted with
// its AC coefficients; under the alternative inter VLC (Annex S) an inter block that does not fit the inter table is read again with Table I.2. Inter
// levels are dequantised as they are read; intra levels in Mpeg4Recon.cpp.
bool Core::block_h263(BitReader& br, Mb& mb, int n, bool coded, bool intra, bool b_part) {
  ArrayView<int16_t> block(b_part ? mb.b_block[n] : mb.block[n]);
  const bool aic = intra && pic_.aic;
  int start = 0, last_pos = -1;
  if (intra && !aic) {
    int dc = static_cast<int>(br.get(8));
    if ((dc & 0x7F) == 0) return false;   // 0 and 128 are not used
    if (dc == 255) dc = 128;
    block[0] = static_cast<int16_t>(dc);
    start = 1;
    last_pos = 0;
  }
  if (coded) {
    const int base = b_part ? b_quant() : qscale_;
    const int q = n < 4 ? base : chroma_quant(base);
    const int qmul = intra ? 1 : q * 2, qadd = intra ? 0 : (q - 1) | 1;
    const int scan_index = aic && mb.ac_pred ? (mb.aic_left ? 2 : 1) : 0;
    const ArrayView<const uint8_t> scan(scan_tables_[scan_index]);
    const BitReader restart = br;
    bool alternative = aic;
    for (int attempt = 0;; ++attempt) {
      const CoefficientTable& t = alternative ? kAicCoefficients : kInterCoefficients;
      const Mem<uint16_t>& lut = alternative ? lut_aic_ : lut_coef_[1];
      int pos = start;
      bool overflow = false, failed = false;
      for (;;) {
        const int sym = read_vlc(br, lut, kCoefficientLen);
        if (sym < 0) {
          failed = true;
          break;
        }
        int run, level, last;
        if (sym == kEscape) {
          last = br.bit();
          run = static_cast<int>(br.get(6));
          level = sign_extend(br.get(8), 8);
          if (level == -128) {
            const int low = static_cast<int>(br.get(5));
            level = low | (sign_extend(br.get(6), 6) * 32);
          }
          if (level == 0) {
            failed = true;
            break;
          }
        } else {
          last = sym >= t.last_start ? 1 : 0;
          run = t.run[sym];
          level = t.level[sym];
          if (br.bit()) level = -level;
        }
        level = level > 0 ? level * qmul + qadd : level * qmul - qadd;
        pos += run;
        if (pos > 63) {
          overflow = true;
          break;
        }
        block[scan[static_cast<uint64_t>(pos)]] = static_cast<int16_t>(level);
        last_pos = pos;
        ++pos;
        if (last) break;
        if (pos > 63) {
          overflow = true;
          break;
        }
        if (br.error()) {
          failed = true;
          break;
        }
      }
      if (!overflow && !failed) break;
      if (overflow && attempt == 0 && !intra && pic_.aiv) {
        br = restart;
        block.fill(0);
        last_pos = -1;
        alternative = true;
        continue;
      }
      return false;
    }
  }
  if (aic) {
    h263_intra_prediction(mb, n);
    last_pos = 63;
  }
  (b_part ? mb.b_last[n] : mb.last[n]) = last_pos;
  return true;
}

// Annex I.3: the prediction of an intra block under advanced intra coding: its DC (and with ac_pred its first row or column) from the block above or to the
// left, if those are intra blocks of the same group of blocks or slice, as FFmpeg's h263_pred_acdc does; the DC is reconstructed here (odd, not negative).
void Core::h263_intra_prediction(Mb& mb, int n) {
  const int comp = n < 4 ? 0 : n - 3;
  const int stride = comp == 0 ? mb_w_ * 2 : mb_w_;
  const int bx = comp == 0 ? mb_x_ * 2 + (n & 1) : mb_x_, by = comp == 0 ? mb_y_ * 2 + (n >> 1) : mb_y_;
  ArrayView<int16_t> dc(dc_[comp].data(), dc_[comp].size());
  ArrayView<int16_t> ac(ac_[comp].data(), ac_[comp].size());
  ArrayView<int16_t> block(mb.block[n]);
  auto available = [&](int x, int y) {
    if (x < 0 || y < 0) return false;
    const int mx = comp == 0 ? x >> 1 : x, my = comp == 0 ? y >> 1 : y;
    if (mx == mb_x_ && my == mb_y_) return true;
    return same_packet(mx, my) && mb_intra_[static_cast<uint64_t>(my * mb_w_ + mx)] != 0;
  };
  const bool has_a = available(bx - 1, by), has_c = available(bx, by - 1);
  const int a = has_a ? dc[static_cast<uint64_t>(by * stride + bx - 1)] : 1024, c = has_c ? dc[static_cast<uint64_t>((by - 1) * stride + bx)] : 1024;
  int pred;
  if (mb.ac_pred) {
    pred = 1024;
    if (mb.aic_left) {
      if (has_a) {
        const ArrayView<int16_t> from = ac.subview(static_cast<uint64_t>(by * stride + bx - 1) * 16, 16);
        for (uint64_t i = 1; i < 8; ++i) block[i * 8] = static_cast<int16_t>(block[i * 8] + from[i]);
        pred = a;
      }
    } else if (has_c) {
      const ArrayView<int16_t> from = ac.subview(static_cast<uint64_t>((by - 1) * stride + bx) * 16, 16);
      for (uint64_t i = 1; i < 8; ++i) block[i] = static_cast<int16_t>(block[i] + from[8 + i]);
      pred = c;
    }
  } else {
    pred = has_a && has_c ? (a + c) >> 1 : has_a ? a : c;
  }
  const int scale = 2 * (n < 4 ? qscale_ : chroma_quant(qscale_));
  int value = block[0] * scale + pred;
  value = value < 0 ? 0 : value | 1;
  block[0] = static_cast<int16_t>(value);
  dc[static_cast<uint64_t>(by * stride + bx)] = static_cast<int16_t>(value);
  const ArrayView<int16_t> to = ac.subview(static_cast<uint64_t>(by * stride + bx) * 16, 16);
  for (uint64_t i = 1; i < 8; ++i) {
    to[i] = block[i * 8];
    to[8 + i] = block[i];
  }
}

}  // namespace mpeg4
}  // namespace hardware
