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


// VC-1 decoding, part 1: the public wrapper, headers, bit planes, the picture pool, reordering and output.
#include "Hardware/VideoCodec/Software/Vc1Core.hpp"

namespace hardware {

Vc1Decoder::~Vc1Decoder() = default;

bool Vc1Decoder::configure(ConstByteView extradata, uint32_t width, uint32_t height) {
  if (!core_) core_ = UniquePointer<vc1::Core>::make(allocator_, allocator_);
  return core_ && core_->configure(extradata, width, height);
}
bool Vc1Decoder::decode(ConstByteView data, int64_t pts_us) {
  if (!core_) core_ = UniquePointer<vc1::Core>::make(allocator_, allocator_);
  return core_ && core_->decode(data, pts_us);
}
bool Vc1Decoder::receive(VideoFrame* frame) { return core_ && core_->receive(frame); }
void Vc1Decoder::flush() {
  if (core_) core_->flush();
}
void Vc1Decoder::reset() {
  if (core_) core_->reset();
}
VideoFormat Vc1Decoder::output_format() const { return core_ ? core_->output_format() : VideoFormat(); }
bool Vc1Decoder::ready() const { return core_ && core_->ready(); }
void Vc1Decoder::set_offload(vc1::IOffload* offload) {
  if (!core_) core_ = UniquePointer<vc1::Core>::make(allocator_, allocator_);
  if (core_) core_->set_offload(offload);
}
void Vc1Decoder::set_frame_observer(vc1::IFrameObserver* observer) {
  if (!core_) core_ = UniquePointer<vc1::Core>::make(allocator_, allocator_);
  if (core_) core_->set_frame_observer(observer);
}

namespace vc1 {

namespace {
#include "Hardware/VideoCodec/Software/Vc1Tables.inc"

constexpr uint32_t kCodeSequence = 0x0F, kCodeEntryPoint = 0x0E, kCodeFrame = 0x0D, kCodeField = 0x0C, kCodeSlice = 0x0B, kCodeEndOfSequence = 0x0A;

// Removes the emulation prevention bytes of an Advanced profile unit: 00 00 03 followed by a byte below 4 stands for 00 00 and that byte (SMPTE 421M E.2.1).
// Returns the number of bytes written; `dst` has room for as many as `src` has.
uint64_t unescape(ConstByteView src, ByteView dst) {
  const uint64_t size = src.size();
  uint64_t out = 0;
  if (size < 4) {
    for (uint64_t i = 0; i < size; ++i) dst[out++] = src[i];
    return out;
  }
  for (uint64_t i = 0; i < size; ++i) {
    if (src[i] == 3 && i >= 2 && src[i - 1] == 0 && src[i - 2] == 0 && i + 1 < size && src[i + 1] < 4) {
      dst[out++] = src[i + 1];
      ++i;
    } else {
      dst[out++] = src[i];
    }
  }
  return out;
}

// The offset of the next 00 00 01 at or after `from`, or the size.
uint64_t next_start_code(ConstByteView data, uint64_t from) {
  for (uint64_t k = from; k + 3 <= data.size(); ++k)
    if (data[k] == 0 && data[k + 1] == 0 && data[k + 2] == 1) return k;
  return data.size();
}
}  // namespace

Core::Core(allocators::IAllocator& allocator) : allocator_(allocator) {
  for (int k = 0; k < 4; ++k)
    for (int i = 0; i < 64; ++i) {
      const int p = kScan8x8[k * 64 + i];
      zz_8x8_[k][i] = static_cast<uint8_t>((p >> 3) | ((p & 7) << 3));   // (transposed: the block holds the coefficients the way the transform wants them)
    }
}

bool Core::build_vlcs() {
  if (vlcs_ready_) return true;
  bool ok = vlc_imode_.build(allocator_, kImodeCode, kImodeBits, 7, 4) && vlc_norm2_.build(allocator_, kNorm2Code, kNorm2Bits, 4, 3) &&
            vlc_norm6_.build(allocator_, kNorm6Code, kNorm6Bits, 64, 9) && vlc_intra_cbpcy_.build(allocator_, kIntraCbpcyCode, kIntraCbpcyBits, 64, 9);
  for (int t = 0; t < 8 && ok; ++t) ok = vlc_ac_[t].build(allocator_, kAcCode + t * 186, kAcBits + t * 186, kAcSize[t], 11);
  for (int t = 0; t < 2 && ok; ++t)
    for (int c = 0; c < 2 && ok; ++c) ok = vlc_dc_[t][c].build(allocator_, kDcCode + (t * 2 + c) * 120, kDcBits + (t * 2 + c) * 120, 120, 9);
  for (int t = 0; t < 4 && ok; ++t) ok = vlc_cbpcy_[t].build(allocator_, kCbpcyCode + t * 64, kCbpcyBits + t * 64, 64, 9) && vlc_mv_diff_[t].build(allocator_, kMvDiffCode + t * 73, kMvDiffBits + t * 73, 73, 9);
  for (int t = 0; t < 3 && ok; ++t)
    ok = vlc_ttmb_[t].build(allocator_, kTtmbCode + t * 16, kTtmbBits + t * 16, 16, 9) && vlc_ttblk_[t].build(allocator_, kTtblkCode + t * 8, kTtblkBits + t * 8, 8, 5) &&
         vlc_subblkpat_[t].build(allocator_, kSubblkpatCode + t * 15, kSubblkpatBits + t * 15, 15, 6);
  vlcs_ready_ = ok;
  return ok;
}

// ---- headers -------------------------------------------------------------------------------------------------------------------------

// 6.1 the Simple and Main profile sequence header (STRUCT_C): 32 bits.
bool Core::parse_sequence_sm(BitReader& br) {
  SequenceHeader s;
  s.profile = static_cast<int>(br.get(2));
  if (s.profile == kAdvanced) return false;
  const bool res_y411 = br.bit() != 0, res_sprite = br.bit() != 0;
  if (res_y411 || res_sprite || s.profile == kComplex) return false;
  s.frmrtq_postproc = static_cast<int>(br.get(3));
  s.bitrtq_postproc = static_cast<int>(br.get(5));
  s.loop_filter = br.bit() != 0;
  s.res_x8 = br.bit() != 0;
  s.multires = br.bit() != 0;
  const bool res_fasttx = br.bit() != 0;
  s.fastuvmc = br.bit() != 0;
  s.extended_mv = br.bit() != 0;
  s.dquant = static_cast<int>(br.get(2));
  s.vstransform = br.bit() != 0;
  const bool res_transtab = br.bit() != 0;
  s.overlap = br.bit() != 0;
  s.resync_marker = br.bit() != 0;
  s.rangered = br.bit() != 0;
  s.max_b_frames = static_cast<int>(br.get(3));
  s.quantizer_mode = static_cast<int>(br.get(2));
  s.finterpflag = br.bit() != 0;
  s.res_rtm_flag = br.bit() != 0;
  if (br.error() || !res_fasttx || res_transtab || !s.res_rtm_flag) return false;
  if (s.profile == kSimple && (!s.fastuvmc || s.extended_mv)) return false;
  s.valid = true;
  s.width = seq_.width;
  s.height = seq_.height;
  seq_ = s;
  return true;
}

// 6.2 the Advanced profile sequence header.
bool Core::parse_sequence_adv(BitReader& br) {
  SequenceHeader s;
  s.profile = static_cast<int>(br.get(2));
  if (s.profile != kAdvanced) return false;
  s.level = static_cast<int>(br.get(3));
  if (br.get(2) != 1) return false;   // chroma format: 4:2:0 only
  s.frmrtq_postproc = static_cast<int>(br.get(3));
  s.bitrtq_postproc = static_cast<int>(br.get(5));
  s.postprocflag = br.bit() != 0;
  s.max_coded_width = (static_cast<int>(br.get(12)) + 1) << 1;
  s.max_coded_height = (static_cast<int>(br.get(12)) + 1) << 1;
  s.broadcast = br.bit() != 0;
  s.interlace = br.bit() != 0;
  s.tfcntrflag = br.bit() != 0;
  s.finterpflag = br.bit() != 0;
  br.skip(1);   // reserved
  s.psf = br.bit() != 0;
  s.max_b_frames = 7;
  s.res_rtm_flag = true;
  if (br.bit()) {   // display extended info: it does not affect the decoding
    br.skip(14 + 14);
    if (br.bit()) {
      if (br.get(4) == 15) br.skip(16);
    }
    if (br.bit()) {
      if (br.bit()) br.skip(16);
      else br.skip(12);
    }
    if (br.bit()) br.skip(24);
  }
  s.hrd_param_flag = br.bit() != 0;
  if (s.hrd_param_flag) {
    s.hrd_num_leaky_buckets = static_cast<int>(br.get(5));
    br.skip(8);
    br.skip(32 * s.hrd_num_leaky_buckets);
  }
  if (br.error() || s.interlace || s.psf) return false;
  s.valid = true;
  s.width = s.max_coded_width;
  s.height = s.max_coded_height;
  seq_ = s;
  return true;
}

// 6.2.2 the entry point header.
bool Core::parse_entry_point(BitReader& br) {
  if (!have_seq_ || seq_.profile != kAdvanced) return false;
  br.skip(1);   // broken link
  br.skip(1);   // closed entry
  seq_.panscanflag = br.bit() != 0;
  seq_.refdist_flag = br.bit() != 0;
  seq_.loop_filter = br.bit() != 0;
  seq_.fastuvmc = br.bit() != 0;
  seq_.extended_mv = br.bit() != 0;
  seq_.dquant = static_cast<int>(br.get(2));
  seq_.vstransform = br.bit() != 0;
  seq_.overlap = br.bit() != 0;
  seq_.quantizer_mode = static_cast<int>(br.get(2));
  if (seq_.hrd_param_flag) br.skip(8 * seq_.hrd_num_leaky_buckets);
  if (br.bit()) {
    seq_.width = (static_cast<int>(br.get(12)) + 1) << 1;
    seq_.height = (static_cast<int>(br.get(12)) + 1) << 1;
  } else {
    seq_.width = seq_.max_coded_width;
    seq_.height = seq_.max_coded_height;
  }
  if (seq_.extended_mv) seq_.extended_dmv = br.bit() != 0;
  seq_.range_mapy_flag = br.bit() != 0;
  if (seq_.range_mapy_flag) br.skip(3);
  seq_.range_mapuv_flag = br.bit() != 0;
  if (seq_.range_mapuv_flag) br.skip(3);
  return !br.error() && !seq_.panscanflag && !seq_.range_mapy_flag && !seq_.range_mapuv_flag;
}

// What follows from a sequence: the size in macroblocks, the buffers of the decoding state, the scans.
bool Core::setup_sequence() {
  sequence_ok_ = false;
  if (!seq_.valid || seq_.width <= 0 || seq_.height <= 0 || seq_.width > 4096 || seq_.height > 4096) return false;
  if (seq_.profile == kAdvanced && ((seq_.width & 1) || (seq_.height & 1))) return false;
  const int mbw = (seq_.width + 15) >> 4, mbh = (seq_.height + 15) >> 4;
  seq_.mb_width = mbw;
  seq_.mb_height = mbh;
  if (!build_vlcs()) return false;
  if (mbw != mb_w_ || mbh != mb_h_ || qscale_.size() == 0) {
    const uint64_t mbs = static_cast<uint64_t>(mbw) * static_cast<uint64_t>(mbh);
    const uint64_t b8 = 4 * mbs;
    bool ok = plane_skip_.alloc(allocator_, mbs) && plane_mvtype_.alloc(allocator_, mbs) && plane_direct_.alloc(allocator_, mbs) && plane_acpred_.alloc(allocator_, mbs) &&
              plane_overflags_.alloc(allocator_, mbs) && slice_start_row_.alloc(allocator_, static_cast<uint64_t>(mbh)) && qscale_.alloc(allocator_, mbs) && intra_mask_.alloc(allocator_, mbs) && cbp_blocks_.alloc(allocator_, mbs) &&
              ttblk_.alloc(allocator_, mbs) && mv_.alloc(allocator_, b8 * 2) && mv_back_.alloc(allocator_, b8 * 2) && uvmv_.alloc(allocator_, mbs * 2) &&
              dc_[0].alloc(allocator_, b8) && dc_[1].alloc(allocator_, mbs) && dc_[2].alloc(allocator_, mbs) && ac_[0].alloc(allocator_, b8 * 16) &&
              ac_[1].alloc(allocator_, mbs * 16) && ac_[2].alloc(allocator_, mbs * 16) && coded_.alloc(allocator_, b8) && blocks_.alloc(allocator_, mbs * 6 * 64);
    if (!ok) return false;
    mb_w_ = mbw;
    mb_h_ = mbh;
    // Pictures of the old size are of no use.
    for (Picture& p : pool_) {
      p.in_use = false;
      p.output_pending = false;
    }
    cur_ = anchor_[0] = anchor_[1] = held_ = returned_ = nullptr;
    queue_count_ = 0;
    need_intra_ = true;
  }
  zz_8x4_ = seq_.profile == kAdvanced ? kScan8x4Adv : kScan8x4Main;
  zz_4x8_ = seq_.profile == kAdvanced ? kScan4x8Adv : kScan4x8Main;
  low_delay_ = seq_.profile != kAdvanced && seq_.max_b_frames == 0;
  // Motion vectors reach as far as the decoded area (Simple and Main profile) or the coded size (Advanced profile).
  ref_w_ = seq_.profile == kAdvanced ? seq_.width : mbw * 16;
  ref_h_ = seq_.profile == kAdvanced ? seq_.height : mbh * 16;
  out_format_ = VideoFormat{static_cast<uint32_t>(seq_.width), static_cast<uint32_t>(seq_.height), PixelFormat::I420};
  sequence_ok_ = true;
  offloaded_ = offload_ && offload_->accepts(seq_);
  return true;
}

bool Core::configure(ConstByteView extradata, uint32_t width, uint32_t height) {
  have_seq_ = false;
  sequence_ok_ = false;
  if (extradata.size() >= 4 && (extradata[0] >> 6) != kAdvanced) {
    if (width == 0 || height == 0 || width > 4096 || height > 4096) return false;
    seq_.width = static_cast<int>(width);
    seq_.height = static_cast<int>(height);
    BitReader br(extradata.first(4));
    if (!parse_sequence_sm(br)) return false;
    have_seq_ = true;
    return setup_sequence();
  }
  // Advanced profile: units with start codes.
  Buf<uint8_t> scratch;
  if (!scratch.alloc(allocator_, extradata.size() + 8)) return false;
  bool any = false;
  uint64_t i = next_start_code(extradata, 0);
  while (i < extradata.size()) {
    const uint64_t end = next_start_code(extradata, i + 3);
    if (i + 4 <= end) {
      const uint32_t code = extradata[i + 3];
      ConstByteView unit = extradata.subview(i + 4, end - (i + 4));
      const uint64_t n = unescape(unit, ByteView(scratch.data(), scratch.size()));
      BitReader br(ConstByteView(scratch.data(), n));
      if (code == kCodeSequence) {
        if (!parse_sequence_adv(br)) return false;
        have_seq_ = true;
        any = true;
      } else if (code == kCodeEntryPoint) {
        if (!parse_entry_point(br)) return false;
      }
    }
    i = end;
  }
  return any && setup_sequence();
}

// 8.7 bit plane coding. `raw`: the plane is not coded here; its bits come with each macroblock.
bool Core::decode_bitplane(BitReader& br, Buf<uint8_t>* plane, bool* raw) {
  const int width = mb_w_, height = mb_h_, stride = mb_w_;
  const bool invert = br.bit() != 0;
  const int imode = vlc_imode_.decode(br);
  *raw = false;
  auto at = [&](int x, int y) -> uint8_t& { return (*plane)[static_cast<uint64_t>(y) * static_cast<uint64_t>(stride) + static_cast<uint64_t>(x)]; };
  plane->fill(0);
  auto rowskip = [&](int x0, int w, int y0, int h) {
    for (int y = y0; y < y0 + h; ++y) {
      if (!br.bit()) {
        for (int x = x0; x < x0 + w; ++x) at(x, y) = 0;
      } else {
        for (int x = x0; x < x0 + w; ++x) at(x, y) = static_cast<uint8_t>(br.bit());
      }
    }
  };
  auto colskip = [&](int x0, int w, int y0, int h) {
    for (int x = x0; x < x0 + w; ++x) {
      if (!br.bit()) {
        for (int y = y0; y < y0 + h; ++y) at(x, y) = 0;
      } else {
        for (int y = y0; y < y0 + h; ++y) at(x, y) = static_cast<uint8_t>(br.bit());
      }
    }
  };
  enum { kRaw = 0, kNorm2, kDiff2, kNorm6, kDiff6, kRowSkip, kColSkip };
  switch (imode) {
    case kRaw:
      *raw = true;
      return true;
    case kDiff2:
    case kNorm2: {
      int x = 0, y = 0;
      int index = 0;
      const int total = width * height;
      auto put = [&](int bit) {
        at(x, y) = static_cast<uint8_t>(bit);
        if (++x == width) {
          x = 0;
          ++y;
        }
      };
      if (total & 1) {
        put(br.bit());
        index = 1;
      }
      for (; index < total; index += 2) {
        const int code = vlc_norm2_.decode(br);
        if (code < 0) return false;
        put(code & 1);
        put(code >> 1);
      }
      break;
    }
    case kDiff6:
    case kNorm6: {
      if (!(height % 3) && (width % 3)) {   // 2 x 3 tiles
        for (int y = 0; y < height; y += 3) {
          for (int x = width & 1; x < width; x += 2) {
            const int code = vlc_norm6_.decode(br);
            if (code < 0) return false;
            at(x, y) = static_cast<uint8_t>(code & 1);
            at(x + 1, y) = static_cast<uint8_t>((code >> 1) & 1);
            at(x, y + 1) = static_cast<uint8_t>((code >> 2) & 1);
            at(x + 1, y + 1) = static_cast<uint8_t>((code >> 3) & 1);
            at(x, y + 2) = static_cast<uint8_t>((code >> 4) & 1);
            at(x + 1, y + 2) = static_cast<uint8_t>((code >> 5) & 1);
          }
        }
        if (width & 1) colskip(0, 1, 0, height);
      } else {   // 3 x 2 tiles
        for (int y = height & 1; y < height; y += 2) {
          for (int x = width % 3; x < width; x += 3) {
            const int code = vlc_norm6_.decode(br);
            if (code < 0) return false;
            at(x, y) = static_cast<uint8_t>(code & 1);
            at(x + 1, y) = static_cast<uint8_t>((code >> 1) & 1);
            at(x + 2, y) = static_cast<uint8_t>((code >> 2) & 1);
            at(x, y + 1) = static_cast<uint8_t>((code >> 3) & 1);
            at(x + 1, y + 1) = static_cast<uint8_t>((code >> 4) & 1);
            at(x + 2, y + 1) = static_cast<uint8_t>((code >> 5) & 1);
          }
        }
        const int x = width % 3;
        if (x) colskip(0, x, 0, height);
        if (height & 1) rowskip(x, width - x, 0, 1);
      }
      break;
    }
    case kRowSkip:
      rowskip(0, width, 0, height);
      break;
    case kColSkip:
      colskip(0, width, 0, height);
      break;
    default:
      return false;
  }
  if (imode == kDiff2 || imode == kDiff6) {
    at(0, 0) ^= static_cast<uint8_t>(invert);
    for (int x = 1; x < width; ++x) at(x, 0) ^= at(x - 1, 0);
    for (int y = 1; y < height; ++y) {
      at(0, y) ^= at(0, y - 1);
      for (int x = 1; x < width; ++x) {
        if (at(x - 1, y) != at(x, y - 1)) at(x, y) ^= static_cast<uint8_t>(invert);
        else at(x, y) ^= at(x - 1, y);
      }
    }
  } else if (invert) {
    for (int y = 0; y < height; ++y)
      for (int x = 0; x < width; ++x) at(x, y) = !at(x, y);
  }
  return !br.error();
}

// 7.1.1.31 VOPDQUANT.
bool Core::decode_dquant(BitReader& br) {
  if (seq_.dquant != 2) {
    pic_.dquantfrm = br.bit() != 0;
    if (!pic_.dquantfrm) return true;
    pic_.dqprofile = static_cast<int>(br.get(2));
    switch (pic_.dqprofile) {
      case kDqSingleEdge:
      case kDqDoubleEdges:
        pic_.dqsbedge = static_cast<int>(br.get(2));
        break;
      case kDqAllMbs:
        pic_.dqbilevel = br.bit() != 0;
        if (!pic_.dqbilevel) {
          pic_.halfpq = false;
          return true;
        }
        break;
      default:
        break;
    }
  } else {
    // dquant == 2: every picture has it (DQUANTFRM is 1, the profile is "all macroblocks"? no: the profile is read below)
  }
  const int pqdiff = static_cast<int>(br.get(3));
  pic_.altpq = pqdiff == 7 ? static_cast<int>(br.get(5)) : pic_.pq + pqdiff + 1;
  return !br.error();
}

bool Core::parse_picture_header(BitReader& br) {
  PictureHeader h;
  const bool adv = seq_.profile == kAdvanced;
  h.rnd = rnd_;
  if (!adv) {
    if (seq_.finterpflag) h.interpfrm = br.bit() != 0;
    br.skip(2);   // frame count
    if (seq_.rangered) h.rangeredfrm = br.bit() != 0;
    if (br.bit()) {
      h.type = kPictureP;
    } else if (seq_.max_b_frames && !br.bit()) {
      h.type = kPictureB;
    } else {
      h.type = kPictureI;
    }
    if (h.type == kPictureB) {
      int index = static_cast<int>(br.get(3));
      if (index == 7) index = 7 + static_cast<int>(br.get(4));
      if (index == 21 || index > 22) return false;
      h.bfraction_index = index;
      h.bfraction = kBfraction[index];
      if (h.bfraction == 0) h.type = kPictureBI;
    }
    if (h.type == kPictureI || h.type == kPictureBI) br.skip(7);   // buffer fullness
    if (h.type == kPictureI || h.type == kPictureBI) h.rnd = 1;
    else if (h.type == kPictureP) h.rnd ^= 1;
  } else {
    // (an interlaced sequence is refused; the frame coding mode is absent)
    switch (br.unary(0, 4)) {
      case 0: h.type = kPictureP; break;
      case 1: h.type = kPictureB; break;
      case 2: h.type = kPictureI; break;
      case 3: h.type = kPictureBI; break;
      default:
        h.type = kPictureP;
        h.skipped = true;
        break;
    }
    if (seq_.tfcntrflag) br.skip(8);
    if (seq_.broadcast) br.skip(2);   // RPTFRM (progressive)
    if (h.skipped) {
      pic_ = h;
      return !br.error();
    }
    h.rnd = br.bit();
    if (seq_.finterpflag) h.interpfrm = br.bit() != 0;
    if (h.type == kPictureB) {
      int index = static_cast<int>(br.get(3));
      if (index == 7) index = 7 + static_cast<int>(br.get(4));
      if (index == 21 || index > 22) return false;
      h.bfraction_index = index;
      h.bfraction = kBfraction[index];
      if (h.bfraction == 0) h.type = kPictureBI;
    }
  }
  const int pqindex = static_cast<int>(br.get(5));
  if (!pqindex) return false;
  h.pqindex = pqindex;
  h.pq = kPquantTable[(seq_.quantizer_mode == kQuantFrameImplicit ? 0 : 1) * 32 + pqindex];
  h.halfpq = pqindex < 9 ? br.bit() != 0 : false;
  switch (seq_.quantizer_mode) {
    case kQuantFrameImplicit: h.pquantizer = pqindex < 9; break;
    case kQuantNonUniform: h.pquantizer = false; break;
    case kQuantFrameExplicit: h.pquantizer = br.bit() != 0; break;
    default: h.pquantizer = true; break;
  }
  pic_ = h;   // (the helpers below read and extend pic_)
  if (adv) {
    if (seq_.postprocflag) pic_.postproc = static_cast<int>(br.get(2));
  } else {
    if (seq_.extended_mv) pic_.mvrange = br.unary(0, 3);
    pic_.k_x = pic_.mvrange + 9 + (pic_.mvrange >> 1);
    pic_.k_y = pic_.mvrange + 8;
    pic_.range_x = 1 << (pic_.k_x - 1);
    pic_.range_y = 1 << (pic_.k_y - 1);
    if (seq_.multires && pic_.type != kPictureB) pic_.respic = static_cast<int>(br.get(2));
    if (seq_.res_x8 && (pic_.type == kPictureI || pic_.type == kPictureBI) && br.bit()) return false;   // the X8 intra coding of Windows Media Image streams
  }
  if (pic_.rangeredfrm || pic_.respic) return false;
  const int type = pic_.type;
  const auto read_planes_and_tables = [&]() -> bool {
    if (!decode_bitplane(br, &plane_skip_, &pic_.skip_raw)) return false;
    pic_.mv_table_index = static_cast<int>(br.get(2));
    pic_.cbptab = static_cast<int>(br.get(2));
    return true;
  };
  const auto read_transform_info = [&]() {
    if (seq_.vstransform) {
      pic_.ttmbf = br.bit() != 0;
      pic_.ttfrm = pic_.ttmbf ? kTtfrmToTt[br.get(2)] : 0;
    } else {
      pic_.ttmbf = true;
      pic_.ttfrm = kTt8x8;
    }
  };
  if (type == kPictureP) {
    if (adv) {
      pic_.mvrange = seq_.extended_mv ? br.unary(0, 3) : 0;
      pic_.k_x = pic_.mvrange + 9 + (pic_.mvrange >> 1);
      pic_.k_y = pic_.mvrange + 8;
      pic_.range_x = 1 << (pic_.k_x - 1);
      pic_.range_y = 1 << (pic_.k_y - 1);
    }
    pic_.tt_index = (pic_.pq > 4) + (pic_.pq > 12);
    const int lowquant = pic_.pq > 12 ? 0 : 1;
    pic_.mv_mode = kMvPmodeTable[lowquant * 5 + br.unary(1, 4)];
    if (pic_.mv_mode == kMvIntensityComp) {
      pic_.mv_mode2 = kMvPmodeTable2[lowquant * 4 + br.unary(1, 3)];
      pic_.lumscale = static_cast<int>(br.get(6));
      pic_.lumshift = static_cast<int>(br.get(6));
      pic_.use_ic = true;
      pic_.quarter_sample = pic_.mv_mode2 != kMv1MvHpel && pic_.mv_mode2 != kMv1MvHpelBilin;
      pic_.mspel = pic_.mv_mode2 != kMv1MvHpelBilin;
    } else {
      pic_.quarter_sample = pic_.mv_mode != kMv1MvHpel && pic_.mv_mode != kMv1MvHpelBilin;
      pic_.mspel = pic_.mv_mode != kMv1MvHpelBilin;
    }
    if ((pic_.mv_mode == kMvIntensityComp && pic_.mv_mode2 == kMvMixed) || pic_.mv_mode == kMvMixed) {
      if (!decode_bitplane(br, &plane_mvtype_, &pic_.mv_type_raw)) return false;
    } else {
      pic_.mv_type_raw = false;
      plane_mvtype_.fill(0);
    }
    if (!read_planes_and_tables()) return false;
    if (seq_.dquant && !decode_dquant(br)) return false;
    read_transform_info();
  } else if (type == kPictureB) {
    if (adv) {
      pic_.mvrange = seq_.extended_mv ? br.unary(0, 3) : 0;
      pic_.k_x = pic_.mvrange + 9 + (pic_.mvrange >> 1);
      pic_.k_y = pic_.mvrange + 8;
      pic_.range_x = 1 << (pic_.k_x - 1);
      pic_.range_y = 1 << (pic_.k_y - 1);
    }
    pic_.tt_index = (pic_.pq > 4) + (pic_.pq > 12);
    pic_.mv_mode = br.bit() ? kMv1Mv : kMv1MvHpelBilin;
    pic_.quarter_sample = pic_.mv_mode == kMv1Mv;
    pic_.mspel = pic_.quarter_sample;
    if (!decode_bitplane(br, &plane_direct_, &pic_.direct_raw)) return false;
    if (!read_planes_and_tables()) return false;
    if (seq_.dquant && !decode_dquant(br)) return false;
    read_transform_info();
  } else if (adv) {   // I and BI of the Advanced profile
    if (!decode_bitplane(br, &plane_acpred_, &pic_.acpred_raw)) return false;
    pic_.condover = kCondOverNone;
    if (seq_.overlap && pic_.pq <= 8) {
      pic_.condover = br.decode012();
      if (pic_.condover == kCondOverSelect && !decode_bitplane(br, &plane_overflags_, &pic_.overflags_raw)) return false;
    }
  }
  pic_.c_ac_table_index = br.decode012();
  if (type == kPictureI || type == kPictureBI) pic_.y_ac_table_index = br.decode012();
  pic_.dc_table_index = br.bit();
  if (adv && (type == kPictureI || type == kPictureBI) && seq_.dquant && !decode_dquant(br)) return false;
  return !br.error();
}

// ---- pictures and output --------------------------------------------------------------------------------------------------------------

VideoFormat Core::output_format() const { return have_seq_ && sequence_ok_ ? out_format_ : VideoFormat(); }

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
  if (held_) output_picture(held_);
  held_ = nullptr;
}

void Core::reset() {
  ic_use_ = false;
  cur_ = nullptr;
  anchor_[0] = anchor_[1] = nullptr;
  held_ = nullptr;
  returned_ = nullptr;
  for (int i = 0; i < queue_count_; ++i) queue_[i]->output_pending = false;
  queue_count_ = 0;
  for (Picture& p : pool_) p.in_use = false;
  need_intra_ = true;
  rnd_ = 0;
}

// Takes a picture from the pool for the one being decoded.
bool Core::start_picture() {
  Picture* p = find_free_picture();
  if (!p) return false;
  const uint64_t w = static_cast<uint64_t>(mb_w_) * 16, h = static_cast<uint64_t>(mb_h_) * 16;
  if (p->width != static_cast<int>(w) || p->height != static_cast<int>(h) || p->plane[0].size() != w * h) {
    if (!p->plane[0].alloc(allocator_, w * h) || !p->plane[1].alloc(allocator_, w * h / 4) || !p->plane[2].alloc(allocator_, w * h / 4) ||
        !p->direct_mv.alloc(allocator_, static_cast<uint64_t>(mb_w_) * static_cast<uint64_t>(mb_h_) * 2))
      return false;
    p->width = static_cast<int>(w);
    p->height = static_cast<int>(h);
    p->stride[0] = static_cast<int>(w);
    p->stride[1] = p->stride[2] = static_cast<int>(w / 2);
  }
  // A picture nobody decodes into stays grey.
  for (int c = 0; c < 3; ++c) ArrayView<uint8_t>(p->plane[c].data(), p->plane[c].size()).fill(128);
  ArrayView<int16_t>(p->direct_mv.data(), p->direct_mv.size()).fill(0);
  p->in_use = true;
  p->output_pending = false;
  p->type = pic_.type;
  p->pts = pts_;
  cur_ = p;
  return true;
}

// ---- the frame -----------------------------------------------------------------------------------------------------------------------

bool Core::decode(ConstByteView data, int64_t pts) {
  pts_ = pts;
  if (data.size() < 1) return false;
  // Units: start code delimited (Advanced profile), or one frame as it is.
  const bool coded = data.size() >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 1;
  Buf<uint8_t> scratch;
  if (!scratch.alloc(allocator_, data.size() + 8)) return false;
  ConstByteView frame;
  bool have_frame = false;
  List<ConstByteView> slices(allocator_), units(allocator_);
  uint64_t used = 0;
  auto stash = [&](ConstByteView unit) -> ConstByteView {
    const uint64_t n = (coded || seq_.profile == kAdvanced) ? unescape(unit, ByteView(scratch.data(), scratch.size()).subview(used)) : 0;
    ConstByteView out = ConstByteView(scratch.data(), scratch.size()).subview(used, n);
    used += n;
    return out;
  };
  if (!coded) {
    if (!have_seq_ || !sequence_ok_) return false;
    if (seq_.profile == kAdvanced) {
      frame = stash(data);
    } else {
      frame = data;
    }
    have_frame = true;
    if (!units.push_back(data)) return false;
  } else {
    uint64_t i = 0;
    while (i < data.size()) {
      const uint64_t start = next_start_code(data, i);
      if (start >= data.size()) break;
      const uint64_t end = next_start_code(data, start + 3);
      if (start + 4 > end) {
        i = end;
        continue;
      }
      const uint32_t code = data[start + 3];
      ConstByteView unit = data.subview(start + 4, end - (start + 4));
      ConstByteView whole = data.subview(start, end - start);
      if (code == kCodeSequence) {
        BitReader br(stash(unit));
        if (!parse_sequence_adv(br)) return false;
        have_seq_ = true;
        if (!setup_sequence()) return false;
      } else if (code == kCodeEntryPoint) {
        BitReader br(stash(unit));
        const int w = seq_.width, h = seq_.height;
        if (!parse_entry_point(br)) return false;
        if (seq_.width != w || seq_.height != h || !sequence_ok_) {
          if (!setup_sequence()) return false;
        }
      } else if (code == kCodeFrame) {
        if (!have_seq_ || !sequence_ok_) return false;
        frame = stash(unit);
        have_frame = true;
        if (!units.push_back(whole)) return false;
      } else if (code == kCodeSlice) {
        if (!have_frame || !slices.push_back(stash(unit)) || !units.push_back(whole)) return false;
      } else if (code == kCodeField) {
        return false;   // interlaced field pictures
      }
      i = end;
    }
  }
  if (!have_frame) return false;
  return decode_frame(frame, slices.view(), units.view());
}

bool Core::decode_frame(ConstByteView frame, ArrayView<const ConstByteView> slices, ArrayView<const ConstByteView> raw_units) {
  BitReader br(frame);
  if (!parse_picture_header(br)) return false;
  // Pictures that cannot be decoded: a stream starts at an intra picture, and a B picture needs both anchors.
  if (need_intra_ && pic_.type != kPictureI) return false;
  if ((pic_.type == kPictureB || pic_.type == kPictureBI) && !anchor_[0]) return false;
  if (pic_.type == kPictureP && !pic_.skipped && !anchor_[1]) return false;
  if (pic_.skipped && !anchor_[1]) return false;
  if (pic_.type != kPictureB && pic_.type != kPictureBI) {
    ic_use_ = pic_.use_ic && pic_.type == kPictureP;
    ic_scale_ = pic_.lumscale;
    ic_shift_ = pic_.lumshift;
  }
  if (!start_picture()) return false;
  if (pic_.type == kPictureI) need_intra_ = false;
  if (pic_.skipped) {
    // The picture repeats the one before it.
    Picture* last = anchor_[1];
    for (int c = 0; c < 3; ++c) ArrayView<uint8_t>(cur_->plane[c].data(), cur_->plane[c].size()).copy_from(ConstByteView(last->plane[c].data(), last->plane[c].size()));
  } else if (!offloaded_) {
    // Per picture state.
    slice_start_row_.fill(0);
    intra_mask_.fill(0);
    cbp_blocks_.fill(0);
    ttblk_.fill(0);
    mv_.fill(0);
    mv_back_.fill(0);
    uvmv_.fill(0);
    qscale_.fill(0);
    esc3_level_length_ = 0;
    esc3_run_length_ = 0;
    ConstByteView data = frame;
    // Switch to the first slice at the macroblock row it names.
    int rows_done = 0;
    int slice_count = static_cast<int>(slices.size());
    for (int s = 0; s <= slice_count; ++s) {
      BitReader sbr = br;
      int start_row = 0, end_row = mb_h_;
      if (s > 0) {
        sbr = BitReader(slices[static_cast<uint64_t>(s - 1)]);
        start_row = static_cast<int>(sbr.get(9));
        if (sbr.bit()) {
          if (!parse_picture_header(sbr)) return false;
        }
      }
      if (s < slice_count) {
        BitReader nbr(slices[static_cast<uint64_t>(s)]);
        end_row = static_cast<int>(nbr.get(9));
      }
      if (end_row > mb_h_) end_row = mb_h_;
      if (start_row >= mb_h_ || end_row <= start_row) continue;
      if (!decode_slice_blocks(sbr, start_row, end_row)) {
        // A damaged slice leaves the rest of its rows as they are (grey, or a prediction).
      }
      rows_done = end_row;
    }
    (void)rows_done;
    (void)data;
    finish_intra_and_filters();
  }
  finish_picture(raw_units);
  return true;
}

void Core::finish_picture(ArrayView<const ConstByteView> units) {
  if (offloaded_ || observer_) {
    OffloadPicture o;
    o.seq = &seq_;
    o.pic = &pic_;
    o.slot = static_cast<int>(cur_ - pool_);
    if (pic_.type == kPictureP && anchor_[1]) o.forward_slot = static_cast<int>(anchor_[1] - pool_);
    if (pic_.type == kPictureB || pic_.type == kPictureBI) {
      if (anchor_[0]) o.forward_slot = static_cast<int>(anchor_[0] - pool_);
      if (anchor_[1]) o.backward_slot = static_cast<int>(anchor_[1] - pool_);
    }
    for (int i = 0; i < 2; ++i)
      if (anchor_[i]) o.anchor_slot[i] = static_cast<int>(anchor_[i] - pool_);
    o.units = units;
    for (int c = 0; c < 3; ++c) {
      o.plane[c] = ByteView(cur_->plane[c].data(), cur_->plane[c].size());
      o.stride[c] = cur_->stride[c];
    }
    if (offloaded_) offload_->decode_picture(o);
    else if (observer_) observer_->picture_decoded(o);
  }
  if (pic_.type == kPictureI || pic_.type == kPictureP) rnd_ = pic_.rnd;
  else if (pic_.type == kPictureBI) rnd_ = pic_.rnd;
  Picture* p = cur_;
  if (pic_.type == kPictureB || pic_.type == kPictureBI) {
    output_picture(p);
  } else if (low_delay_) {
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

}  // namespace vc1
}  // namespace hardware
