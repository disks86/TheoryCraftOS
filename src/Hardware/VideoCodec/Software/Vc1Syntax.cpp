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

Core::Core(allocators::IAllocator& allocator) : allocator_(allocator), unit_infos_(allocator) {
  for (int k = 0; k < 4; ++k)
    for (int i = 0; i < 64; ++i) {
      const int p = kScan8x8[k * 64 + i];
      zz_8x8_[k][i] = static_cast<uint8_t>((p >> 3) | ((p & 7) << 3));   // (transposed: the block holds the coefficients the way the transform wants them)
    }
  for (int i = 0; i < 64; ++i) zzi_8x8_[i] = static_cast<uint8_t>((kScan8x8Interlaced[i] >> 3) | ((kScan8x8Interlaced[i] & 7) << 3));
  IcLuts* const all_luts[3] = {&ic_last_, &ic_next_, &ic_aux_};
  for (IcLuts* l : all_luts) {
    init_lut(32, 0, *l, 0, false);
    init_lut(32, 0, *l, 1, false);
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
  // interlaced pictures
  for (int t = 0; t < 8 && ok; ++t)
    ok = vlc_if_mmv_mbmode_[t].build(allocator_, kIfMmvMbmodeCode + t * 8, kIfMmvMbmodeBits + t * 8, 8, 6) &&
         vlc_if_1mv_mbmode_[t].build(allocator_, kIf1MvMbmodeCode + t * 6, kIf1MvMbmodeBits + t * 6, 6, 5) &&
         vlc_mvdata2_[t].build(allocator_, kMvdata2RefCode + t * 126, kMvdata2RefBits + t * 126, 126, 9) &&
         vlc_icbpcy_[t].build(allocator_, kIcbpcyCode + t * 63, kIcbpcyBits + t * 63, 63, 9);
  for (int t = 0; t < 4 && ok; ++t)
    ok = vlc_mvdata1_[t].build(allocator_, kMvdata1RefCode + t * 72, kMvdata1RefBits + t * 72, 72, 9) &&
         vlc_4mvbp_[t].build(allocator_, k4MvBpCode + t * 16, k4MvBpBits + t * 16, 16, 6) && vlc_2mvbp_[t].build(allocator_, k2MvBpCode + t * 4, k2MvBpBits + t * 4, 4, 3) &&
         vlc_intfr_4mv_mbmode_[t].build(allocator_, kIntfr4MvMbmodeCode + t * 15, kIntfr4MvMbmodeBits + t * 15, 15, 9) &&
         vlc_intfr_non4mv_mbmode_[t].build(allocator_, kIntfrNon4MvMbmodeCode + t * 9, kIntfrNon4MvMbmodeBits + t * 9, 9, 6);
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
  if (br.error() || s.psf) return false;   // (progressive segmented frames: only a display matter, but no reference decoder has them to check against)
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
              plane_overflags_.alloc(allocator_, mbs) && plane_forward_.alloc(allocator_, mbs) && slice_start_row_.alloc(allocator_, static_cast<uint64_t>(mbh)) &&
              qscale_.alloc(allocator_, mbs) && intra_mask_.alloc(allocator_, mbs) && cbp_blocks_.alloc(allocator_, mbs) &&
              ttblk_.alloc(allocator_, mbs) && mv_.alloc(allocator_, b8 * 2) && mv_back_.alloc(allocator_, b8 * 2) && mv_f_[0].alloc(allocator_, b8) && mv_f_[1].alloc(allocator_, b8) &&
              uvmv_.alloc(allocator_, mbs * 2) && uvmv_f_.alloc(allocator_, mbs) &&
              dc_[0].alloc(allocator_, b8) && dc_[1].alloc(allocator_, mbs) && dc_[2].alloc(allocator_, mbs) && ac_[0].alloc(allocator_, b8 * 16) &&
              ac_[1].alloc(allocator_, mbs * 16) && ac_[2].alloc(allocator_, mbs * 16) && coded_.alloc(allocator_, b8) && blocks_.alloc(allocator_, mbs * 6 * 64);
    if (!ok) return false;
    mb_w_ = mbw;
    mb_h_ = mbh;
    mb_rows_ = mbh;
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
  const int width = mb_w_, height = mb_rows_, stride = mb_w_;   // (a field picture's plane has the field's rows)
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

// ---- intensity compensation tables ------------------------------------------------------------------------------------------------------

namespace {
int clip255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
}  // namespace

// The tables of one reference field (or, of a frame, the lines of one parity) for LUMSCALE and LUMSHIFT (8.3.8). `chain`: they apply to what the tables give
// already - a reference compensated before is compensated again, as the reference decoder does it -, otherwise to the samples themselves.
void Core::init_lut(int lumscale, int lumshift, IcLuts& luts, int field, bool chain) {
  int scale, shift;
  if (!lumscale) {
    scale = -64;
    shift = (255 - lumshift * 2) * 64;
    if (lumshift > 31) shift += 128 << 6;
  } else {
    scale = lumscale + 32;
    shift = lumshift > 31 ? (lumshift - 64) * 64 : lumshift << 6;
  }
  ArrayView<uint8_t> y(luts.y[field & 1], 256), uv(luts.uv[field & 1], 256);
  for (int i = 0; i < 256; ++i) {
    const int iy = chain ? y[static_cast<uint64_t>(i)] : i;
    const int iu = chain ? uv[static_cast<uint64_t>(i)] : i;
    y[static_cast<uint64_t>(i)] = static_cast<uint8_t>(clip255((scale * iy + shift + 32) >> 6));
    uv[static_cast<uint64_t>(i)] = static_cast<uint8_t>(clip255((scale * (iu - 128) + 128 * 64 + 32) >> 6));
  }
}

// At the first header of every picture (not a slice's, not a second field's; nor a skipped picture's): an I or P picture becomes the backward anchor, so the tables
// of the anchor before it (what its own second field did to its first, or nothing) become the forward ones, and the current frame starts uncompensated; a B
// picture keeps both and has tables of its own for its first field.
void Core::rotate_luts(int type) {
  if (type == kPictureB || type == kPictureBI) {
    ic_curr_is_aux_ = true;
  } else {
    const IcLuts last = ic_last_;
    ic_last_ = ic_next_;
    ic_next_ = last;
    ic_curr_is_aux_ = false;
  }
  IcLuts& c = ic_curr();
  init_lut(32, 0, c, 0, false);
  init_lut(32, 0, c, 1, false);
  c.use = false;
}

// ---- the picture header ----------------------------------------------------------------------------------------------------------------

// 7.1 (progressive) and 9.1 (interlaced) the picture header, read from `at`; leaves it in pic_ (and the bit planes in the Core).
bool Core::parse_picture_header(BitReader& br, HeaderAt at) {
  PictureHeader h;
  const bool adv = seq_.profile == kAdvanced;
  if (at == HeaderAt::SecondField) {
    // The second field of a field pair: the frame's part is the first field's; the type is the second one of FPTYPE; the header goes on at PQINDEX.
    if (pic_.fcm != kFcmFieldInterlace || !field_mode_) return false;
    h.fcm = pic_.fcm;
    h.fptype = pic_.fptype;
    h.tff = pic_.tff;
    h.rff = pic_.rff;
    h.rptfrm = pic_.rptfrm;
    h.uvsamp = pic_.uvsamp;
    h.rnd = pic_.rnd;
    h.interpfrm = pic_.interpfrm;
    h.bfraction_index = pic_.bfraction_index;
    h.bfraction = pic_.bfraction;
    h.refdist = pic_.refdist;
    h.frfd = pic_.frfd;
    h.brfd = pic_.brfd;
    if (h.fptype & 4) h.type = (h.fptype & 1) ? kPictureBI : kPictureB;
    else h.type = (h.fptype & 1) ? kPictureP : kPictureI;
  } else if (!adv) {
    field_mode_ = false;
    mb_rows_ = mb_h_;
    h.rnd = rnd_;
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
    // In an interlaced sequence the frame starts with its coding mode: 0 progressive, 10 frame interlace, 11 field interlace (two field pictures).
    const int fcm = seq_.interlace ? br.decode012() : kFcmProgressive;
    const bool field = fcm == kFcmFieldInterlace;
    if (at == HeaderAt::Slice && field != field_mode_) return false;   // (a slice's header cannot change the frame's coding mode)
    h.fcm = fcm;
    field_mode_ = field;
    mb_rows_ = field ? (mb_h_ + 1) / 2 : mb_h_;   // (a field has half the rows: the frame's rounded up to even, halved)
    int first_field_type = kPictureI;
    if (field) {
      // FPTYPE: bit 2 says the fields are B pictures, bit 1 that the first is the second kind (BI, P), bit 0 the same for the second field. `type` is the frame's
      // class while only the frame is described (an I frame has an intra first field and can be decoded alone, whatever its second field; a B frame is BI only
      // when both fields are), the first field's type once the fields are decoded here.
      h.fptype = static_cast<int>(br.get(3));
      const bool b_fields = (h.fptype & 4) != 0, first_other = (h.fptype & 2) != 0, second_other = (h.fptype & 1) != 0;
      if (b_fields) h.type = first_other && second_other ? kPictureBI : kPictureB;
      else h.type = first_other ? kPictureP : kPictureI;
      first_field_type = b_fields ? (first_other ? kPictureBI : kPictureB) : h.type;
    } else {
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
    }
    if (seq_.tfcntrflag) br.skip(8);
    if (seq_.broadcast) {
      if (!seq_.interlace || seq_.psf) {
        h.rptfrm = static_cast<int>(br.get(2));
      } else {
        h.tff = br.bit() != 0;
        h.rff = br.bit() != 0;
      }
    }
    if (h.skipped) {
      pic_ = h;
      return !br.error();
    }
    h.rnd = br.bit();
    if (seq_.interlace) h.uvsamp = br.bit() != 0;
    // Frame interlace pictures, and field pictures that an engine (or a DXVA driver) decodes: the rest of the header, the bit planes and the macroblocks are theirs.
    if (h.fcm != kFcmProgressive && (offloaded_ || ext_ || h.fcm == kFcmFrameInterlace)) {
      pic_ = h;
      return !br.error();
    }
    if (field) {
      h.type = first_field_type;
      // REFDIST: read in a frame of I and P fields (0 without REFDIST_FLAG); a B field pair uses the last one sent.
      if (!seq_.refdist_flag) {
        refdist_ = 0;
      } else if (h.type != kPictureB && h.type != kPictureBI) {
        int refdist = static_cast<int>(br.get(2));
        if (refdist == 3) refdist += br.unary(0, 14);
        if (refdist > 16) return false;
        refdist_ = refdist;
      }
      h.refdist = refdist_;
      if (h.type == kPictureB || h.type == kPictureBI) {
        int index = static_cast<int>(br.get(3));
        if (index == 7) index = 7 + static_cast<int>(br.get(4));
        if (index == 21 || index > 22) return false;
        h.bfraction_index = index;
        h.bfraction = kBfraction[index];
        h.frfd = (h.bfraction * h.refdist) >> 8;
        h.brfd = h.refdist - h.frfd - 1;
        if (h.brfd < 0) h.brfd = 0;
      }
    } else {
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
  }
  // Which field this is: the top one is first when TFF is set.
  if (field_mode_) cur_field_ = h.tff == second_field_ ? 1 : 0;
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
  if (at == HeaderAt::Frame) rotate_luts(pic_.type);
  const int type = pic_.type;
  const bool field = field_mode_;
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
  const auto set_ranges = [&]() {
    pic_.k_x = pic_.mvrange + 9 + (pic_.mvrange >> 1);
    pic_.k_y = pic_.mvrange + 8;
    pic_.range_x = 1 << (pic_.k_x - 1);
    pic_.range_y = 1 << (pic_.k_y - 1);
  };
  if (type == kPictureP) {
    if (field) {
      pic_.numref = br.bit();
      if (!pic_.numref) {
        pic_.reffield = br.bit();
        ref_field_[0] = pic_.reffield ^ (cur_field_ ^ 1);
      }
    }
    if (adv) {
      pic_.mvrange = seq_.extended_mv ? br.unary(0, 3) : 0;
      if (seq_.interlace) pic_.dmvrange = seq_.extended_dmv ? br.unary(0, 3) : 0;   // DMVRANGE (only the interlaced pictures use it)
      set_ranges();
    }
    pic_.tt_index = (pic_.pq > 4) + (pic_.pq > 12);
    const int lowquant = pic_.pq > 12 ? 0 : 1;
    pic_.mv_mode = kMvPmodeTable[lowquant * 5 + br.unary(1, 4)];
    if (pic_.mv_mode == kMvIntensityComp) {
      pic_.mv_mode2 = kMvPmodeTable2[lowquant * 4 + br.unary(1, 3)];
      // INTCOMPFIELD (field pictures): which reference fields are compensated, 1 both, 01 the bottom one, 00 the top one.
      if (field) {
        const int code = br.bit() ? 0 : 2 - br.bit();
        pic_.intcompfield = code ^ 3;
      } else {
        pic_.intcompfield = 3;
      }
      pic_.lumscale = pic_.lumscale2 = 32;
      pic_.lumshift = pic_.lumshift2 = 0;
      if (pic_.intcompfield & 1) {
        pic_.lumscale = static_cast<int>(br.get(6));
        pic_.lumshift = static_cast<int>(br.get(6));
      }
      if ((pic_.intcompfield & 2) && field) {
        pic_.lumscale2 = static_cast<int>(br.get(6));
        pic_.lumshift2 = static_cast<int>(br.get(6));
      } else if (!field) {
        pic_.lumscale2 = pic_.lumscale;
        pic_.lumshift2 = pic_.lumshift;
      }
      pic_.use_ic = true;
      // The tables: the first field (and a frame) compensates both fields of the forward reference, on top of what they had. The second field of a field pair
      // compensates the first field of its own frame (the opposite parity; it starts from the samples) and the field of the same parity of the reference frame
      // (on top of what the first field did to it). The first scale and shift are the top field's, the second ones the bottom field's.
      if (field && second_field_) {
        if (cur_field_) {
          init_lut(pic_.lumscale, pic_.lumshift, ic_curr(), 0, false);
          init_lut(pic_.lumscale2, pic_.lumshift2, ic_last_, 1, true);
        } else {
          init_lut(pic_.lumscale2, pic_.lumshift2, ic_curr(), 1, false);
          init_lut(pic_.lumscale, pic_.lumshift, ic_last_, 0, true);
        }
        ic_next_.use = true;
        ic_curr().use = true;
      } else {
        init_lut(pic_.lumscale, pic_.lumshift, ic_last_, 0, true);
        init_lut(pic_.lumscale2, pic_.lumshift2, ic_last_, 1, true);
      }
      ic_last_.use = true;
      pic_.quarter_sample = pic_.mv_mode2 != kMv1MvHpel && pic_.mv_mode2 != kMv1MvHpelBilin;
      pic_.mspel = pic_.mv_mode2 != kMv1MvHpelBilin;
    } else {
      pic_.quarter_sample = pic_.mv_mode != kMv1MvHpel && pic_.mv_mode != kMv1MvHpelBilin;
      pic_.mspel = pic_.mv_mode != kMv1MvHpelBilin;
    }
    const bool mixed = (pic_.mv_mode == kMvIntensityComp && pic_.mv_mode2 == kMvMixed) || pic_.mv_mode == kMvMixed;
    if (field) {
      pic_.mbmodetab = static_cast<int>(br.get(3));
      pic_.imvtab = static_cast<int>(br.get(2 + pic_.numref));
      pic_.icbptab = static_cast<int>(br.get(3));
      if (mixed) pic_.fourmvbptab = static_cast<int>(br.get(2));
    } else {
      if (mixed) {
        if (!decode_bitplane(br, &plane_mvtype_, &pic_.mv_type_raw)) return false;
      } else {
        pic_.mv_type_raw = false;
        plane_mvtype_.fill(0);
      }
      if (!read_planes_and_tables()) return false;
    }
    if (seq_.dquant && !decode_dquant(br)) return false;
    read_transform_info();
  } else if (type == kPictureB) {
    if (adv) {
      pic_.mvrange = seq_.extended_mv ? br.unary(0, 3) : 0;
      set_ranges();
    }
    pic_.tt_index = (pic_.pq > 4) + (pic_.pq > 12);
    if (field) {
      if (seq_.extended_dmv) pic_.dmvrange = br.unary(0, 3);
      const int lowquant = pic_.pq > 12 ? 0 : 1;
      pic_.mv_mode = kMvPmodeTable2[lowquant * 4 + br.unary(1, 3)];
      pic_.quarter_sample = pic_.mv_mode == kMv1Mv || pic_.mv_mode == kMvMixed;
      pic_.mspel = pic_.mv_mode != kMv1MvHpelBilin;
      if (!decode_bitplane(br, &plane_forward_, &pic_.forward_raw)) return false;
      pic_.mbmodetab = static_cast<int>(br.get(3));
      pic_.imvtab = static_cast<int>(br.get(3));
      pic_.icbptab = static_cast<int>(br.get(3));
      if (pic_.mv_mode == kMvMixed) pic_.fourmvbptab = static_cast<int>(br.get(2));
      pic_.numref = 1;   // (a B field always has two reference fields per direction)
    } else {
      pic_.mv_mode = br.bit() ? kMv1Mv : kMv1MvHpelBilin;
      pic_.quarter_sample = pic_.mv_mode == kMv1Mv;
      pic_.mspel = pic_.quarter_sample;
      if (!decode_bitplane(br, &plane_direct_, &pic_.direct_raw)) return false;
      if (!read_planes_and_tables()) return false;
    }
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
  if (type == kPictureI || type == kPictureBI) {
    pic_.y_ac_table_index = br.decode012();
  } else if (pic_.fcm != kFcmProgressive && !pic_.quarter_sample) {
    pic_.range_x <<= 1;   // (interlaced pictures with half sample vectors: the range is in half samples)
    pic_.range_y <<= 1;
  }
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
  IcLuts* const all_luts[3] = {&ic_last_, &ic_next_, &ic_aux_};
  for (IcLuts* l : all_luts) {
    init_lut(32, 0, *l, 0, false);
    init_lut(32, 0, *l, 1, false);
    l->use = false;
  }
  ic_curr_is_aux_ = false;
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
  Picture* p = ext_ ? ext_->target : find_free_picture();
  if (!p) return false;
  // Two field pictures need their rows in whole macroblocks per field: the frame's macroblock rows rounded up to even.
  const bool fields = pic_.fcm == kFcmFieldInterlace && !offloaded_;
  const int rows = fields ? (mb_h_ + 1) & ~1 : mb_h_;
  const uint64_t w = static_cast<uint64_t>(mb_w_) * 16, h = static_cast<uint64_t>(rows) * 16;
  const uint64_t mbs = static_cast<uint64_t>(mb_w_) * static_cast<uint64_t>(rows);
  // (mb_flags is a field pair's only: a DXVA driver's pictures, which it sizes itself, have none)
  if (p->width != static_cast<int>(w) || p->height != static_cast<int>(h) || p->plane[0].size() != w * h || p->direct_mv.size() != mbs * 2 || (fields && p->mb_flags.size() != mbs)) {
    if (!p->plane[0].alloc(allocator_, w * h) || !p->plane[1].alloc(allocator_, w * h / 4) || !p->plane[2].alloc(allocator_, w * h / 4) ||
        !p->direct_mv.alloc(allocator_, mbs * 2) || (fields && !p->mb_flags.alloc(allocator_, mbs)))
      return false;
    p->width = static_cast<int>(w);
    p->height = static_cast<int>(h);
    p->stride[0] = static_cast<int>(w);
    p->stride[1] = p->stride[2] = static_cast<int>(w / 2);
  }
  // A picture nobody decodes into stays grey.
  for (int c = 0; c < 3; ++c) ArrayView<uint8_t>(p->plane[c].data(), p->plane[c].size()).fill(128);
  ArrayView<int16_t>(p->direct_mv.data(), p->direct_mv.size()).fill(0);
  ArrayView<uint8_t>(p->mb_flags.data(), p->mb_flags.size()).fill(0);
  p->in_use = true;
  p->output_pending = false;
  p->type = pic_.type;
  p->pts = pts_;
  p->interlaced = pic_.fcm != kFcmProgressive;
  p->field_coded = pic_.fcm == kFcmFieldInterlace;
  cur_ = p;
  // The picture being decoded is the frame (a field picture changes this, see begin_field).
  for (int c = 0; c < 3; ++c) {
    dst_off_[c] = 0;
    dst_stride_[c] = p->stride[c];
  }
  return true;
}

// The decoding state of a picture (or a field) that starts.
void Core::reset_picture_state() {
  slice_start_row_.fill(0);
  intra_mask_.fill(0);
  cbp_blocks_.fill(0);
  ttblk_.fill(0);
  mv_.fill(0);
  mv_back_.fill(0);
  mv_f_[0].fill(0);
  mv_f_[1].fill(0);
  uvmv_.fill(0);
  uvmv_f_.fill(0);
  qscale_.fill(0);
  esc3_level_length_ = 0;
  esc3_run_length_ = 0;
}

// ---- the frame -----------------------------------------------------------------------------------------------------------------------

bool Core::decode(ConstByteView data, int64_t pts) {
  pts_ = pts;
  if (data.size() < 1) return false;
  // Units: start code delimited (Advanced profile), or one frame as it is.
  const bool coded = data.size() >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 1;
  Buf<uint8_t> scratch;
  if (!scratch.alloc(allocator_, data.size() + 8)) return false;
  bool have_frame = false;
  List<Part> parts(allocator_);
  List<ConstByteView> units(allocator_);
  uint64_t used = 0;
  auto stash = [&](ConstByteView unit) -> ConstByteView {
    const uint64_t n = (coded || seq_.profile == kAdvanced) ? unescape(unit, ByteView(scratch.data(), scratch.size()).subview(used)) : 0;
    ConstByteView out = ConstByteView(scratch.data(), scratch.size()).subview(used, n);
    used += n;
    return out;
  };
  if (!coded) {
    if (!have_seq_ || !sequence_ok_) return false;
    Part frame;
    frame.code = kCodeFrame;
    frame.data = seq_.profile == kAdvanced ? stash(data) : data;
    frame.raw = data;
    have_frame = true;
    if (!parts.push_back(frame) || !units.push_back(data)) return false;
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
      } else if (code == kCodeFrame || code == kCodeSlice || code == kCodeField) {
        // The frame, its slices and (field interlace) the second field, in order. A second frame is not this one's.
        if (code == kCodeFrame) {
          if (have_frame) break;
          if (!have_seq_ || !sequence_ok_) return false;
          have_frame = true;
        } else if (!have_frame) {
          return false;
        }
        Part part;
        part.code = code;
        part.data = stash(unit);
        part.raw = whole;
        if (!parts.push_back(part) || !units.push_back(whole)) return false;
      }
      i = end;
    }
  }
  if (!have_frame) return false;
  return decode_frame(parts.view(), units.view());
}

bool Core::decode_frame(ArrayView<const Part> parts, ArrayView<const ConstByteView> raw_units) {
  BitReader br(parts[0].data);
  second_field_ = false;
  if (!parse_picture_header(br, HeaderAt::Frame)) return false;
  // (DXVA: the rounding control of a Simple or Main profile picture is the one the program says; it follows from pictures the decoder may not have seen)
  if (ext_ && ext_->rnd >= 0 && seq_.profile != kAdvanced) pic_.rnd = ext_->rnd & 1;
  // A frame interlace picture has no pixel path here: only an engine can decode it. (Field pictures are decoded here, but not for a DXVA driver.)
  if (pic_.fcm != kFcmProgressive && (ext_ || (pic_.fcm == kFcmFrameInterlace && !offloaded_))) return false;
  if (ext_) {
    // The references are the caller's: every one the picture type needs must be there.
    anchor_[0] = anchor_[1] = nullptr;
    if (pic_.type == kPictureP) {
      if (!ext_->forward) return false;
      anchor_[1] = ext_->forward;
    } else if (pic_.type == kPictureB) {
      if (!ext_->forward || !ext_->backward) return false;
      anchor_[0] = ext_->forward;
      anchor_[1] = ext_->backward;
    }
    if (pic_.type == kPictureB) {
      ic_use_ = ext_->ic_forward;
      ic_scale_ = ext_->ic_scale;
      ic_shift_ = ext_->ic_shift;
    } else {
      ic_use_ = pic_.use_ic && pic_.type == kPictureP;
      ic_scale_ = pic_.lumscale;
      ic_shift_ = pic_.lumshift;
    }
    // The tables of the references are the ones the caller says (the history the decoder keeps for them is not this picture's).
    IcLuts* const all_luts[3] = {&ic_last_, &ic_next_, &ic_aux_};
    for (IcLuts* l : all_luts) {
      init_lut(32, 0, *l, 0, false);
      init_lut(32, 0, *l, 1, false);
      l->use = false;
    }
    if (ic_use_) {
      init_lut(ic_scale_, ic_shift_, ic_last_, 0, false);
      init_lut(ic_scale_, ic_shift_, ic_last_, 1, false);
      ic_last_.use = true;
    }
  } else {
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
  }
  if (!start_picture()) return false;
  if (pic_.type == kPictureI) need_intra_ = false;
  if (pic_.skipped) {
    // The picture repeats the one before it.
    Picture* last = anchor_[1];
    for (int c = 0; c < 3; ++c) ArrayView<uint8_t>(cur_->plane[c].data(), cur_->plane[c].size()).copy_from(ConstByteView(last->plane[c].data(), last->plane[c].size()));
  } else if (pic_.fcm == kFcmFieldInterlace && !offloaded_) {
    // Two field pictures; what describes the frame afterwards is its first field's header.
    const PictureHeader frame_header = pic_;
    decode_field_pair(br, parts);
    pic_ = frame_header;
    field_mode_ = false;
    second_field_ = false;
    mb_rows_ = mb_h_;
  } else {
    if (!offloaded_) reset_picture_state();
    if (!walk_progressive_slices(br, parts)) return false;
    if (!offloaded_) finish_intra_and_filters();
  }
  finish_picture(raw_units);
  return true;
}

// The frame unit and its slices (with the pixel work offloaded they are only walked, to know where their macroblocks begin). False: a slice header is damaged.
bool Core::walk_progressive_slices(BitReader& br, ArrayView<const Part> parts) {
  unit_infos_.clear();
  UnitInfo first;
  first.mb_bit_offset = static_cast<uint32_t>(br.position());
  if (!unit_infos_.push_back(first)) return false;
  // The slices, in order (a field unit of a field pair that an engine decodes is the engine's).
  List<ConstByteView> slice_list(allocator_);
  for (const Part& p : parts)
    if (p.code == kCodeSlice && !slice_list.push_back(p.data)) return false;
  const ArrayView<ConstByteView> slices = slice_list.view();
  const int slice_count = static_cast<int>(slices.size());
  for (int s = 0; s <= slice_count; ++s) {
    BitReader sbr = br;
    int start_row = 0, end_row = mb_h_;
    if (s > 0) {
      sbr = BitReader(slices[static_cast<uint64_t>(s - 1)]);
      start_row = static_cast<int>(sbr.get(9));
      if (sbr.bit()) {
        if (!parse_picture_header(sbr, HeaderAt::Slice)) return false;
      }
      UnitInfo info;
      info.start_row = start_row;
      info.mb_bit_offset = static_cast<uint32_t>(sbr.position());
      if (!unit_infos_.push_back(info)) return false;
    }
    if (offloaded_) continue;
    if (s < slice_count) {
      BitReader nbr(slices[static_cast<uint64_t>(s)]);
      end_row = static_cast<int>(nbr.get(9));
    }
    if (end_row > mb_h_) end_row = mb_h_;
    if (start_row >= mb_h_ || end_row <= start_row) continue;
    if (!decode_slice_blocks(sbr, start_row, end_row)) {
      // A damaged slice leaves the rest of its rows as they are (grey, or a prediction).
    }
  }
  return true;
}

// The decoding state of a field that starts: its parity, where its lines are in the frame, the per picture state.
void Core::begin_field(bool second) {
  second_field_ = second;
  field_mode_ = true;
  cur_field_ = pic_.tff == second ? 1 : 0;
  mb_rows_ = (mb_h_ + 1) / 2;
  for (int c = 0; c < 3; ++c) {
    dst_stride_[c] = cur_->stride[c] * 2;
    dst_off_[c] = cur_field_ ? static_cast<uint64_t>(cur_->stride[c]) : 0;
  }
  reset_picture_state();
}

// What follows the macroblocks of a field: its overlap smoothing, intra samples and in-loop filter (the second field predicts from the finished first one).
void Core::finish_field() { finish_intra_and_filters(); }

// 9.1: a frame of two field pictures. The frame unit holds the first field (and its slices follow it), the field unit (start code 0x0C) the second field's header
// and macroblocks (and its slices follow it). The units are taken in order the way the reference decoder takes them: a slice belongs to the second field when its
// address (SLICE_ADDR, counted on from the first field's rows in the second field) is past the first field, and a slice's rows end where the next unit's begin.
void Core::decode_field_pair(BitReader& br, ArrayView<const Part> parts) {
  const int rows = (mb_h_ + 1) / 2;
  const int64_t n_slices = static_cast<int64_t>(parts.size()) - 1;   // the units after the frame unit (slices and field units)
  // The index (among those units) of the last field unit, less one: the last unit of the first field (-1 without a field unit).
  int64_t n_slices1 = -1;
  for (int64_t k = 0; k < n_slices; ++k)
    if (parts[static_cast<uint64_t>(k + 1)].code == kCodeField) n_slices1 = k - 1;
  // The first macroblock row (as an address) of the unit after the frame unit with index k.
  auto address = [&](int64_t k) -> int {
    const Part& p = parts[static_cast<uint64_t>(k + 1)];
    if (p.code == kCodeField) return rows;
    BitReader a(p.data);
    return static_cast<int>(a.get(9));
  };
  unit_infos_.clear();
  begin_field(false);
  bool header_ok = true;
  for (int64_t i = 0; i <= n_slices; ++i) {
    BitReader rbr = br;
    if (i > 0) {
      rbr = BitReader(parts[static_cast<uint64_t>(i)].data);
      if (parts[static_cast<uint64_t>(i)].code == kCodeSlice) rbr.skip(9);
    }
    const bool second = i > 0 && address(i - 1) >= rows;
    if (second != second_field_) {
      if (!second) continue;   // (a slice of the first field after the second one began: no stream has that)
      finish_field();
      begin_field(true);
    }
    if (i > 0) {
      if (i == n_slices1 + 2) header_ok = parse_picture_header(rbr, HeaderAt::SecondField);
      else if (rbr.bit()) header_ok = parse_picture_header(rbr, HeaderAt::Slice);
    }
    if (!header_ok) continue;   // (a damaged header: the units up to the next good one are skipped)
    UnitInfo info;
    info.start_row = i == 0 ? 0 : address(i - 1);
    info.mb_bit_offset = static_cast<uint32_t>(rbr.position());
    if (!unit_infos_.push_back(info)) return;
    int start = i == 0 ? 0 : address(i - 1) % rows;
    if (start < 0) start = 0;
    int end;
    if (second_field_) {
      end = i == n_slices ? rows : address(i) % rows;
    } else {
      if (i >= n_slices) continue;   // (a first field without a second one is not decoded, as by the reference decoder)
      end = i == n_slices1 + 1 ? rows : address(i) % rows;
    }
    if (end > rows) end = rows;
    if (end <= start) continue;
    if (!decode_slice_blocks(rbr, start, end)) {
      // A damaged slice leaves the rest of its rows as they are.
    }
  }
  finish_field();
}

void Core::finish_picture(ArrayView<const ConstByteView> units) {
  if (ext_) {   // (the caller keeps the picture; nothing is output)
    cur_ = nullptr;
    return;
  }
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
    o.unit_info = unit_infos_.view();
    if (!pic_.skipped && pic_.fcm == kFcmProgressive) {   // (an interlaced picture's bit planes are not described)
      const bool intra_like = pic_.type == kPictureI || pic_.type == kPictureBI;
      const bool adv = seq_.profile == kAdvanced;
      auto plane = [&](const Buf<uint8_t>& b) { return ArrayView<const uint8_t>(b.data(), b.size()); };
      if (pic_.type == kPictureP) {
        if (!pic_.skip_raw) o.skip_plane = plane(plane_skip_);
        if (!pic_.mv_type_raw && ((pic_.mv_mode == kMvIntensityComp && pic_.mv_mode2 == kMvMixed) || pic_.mv_mode == kMvMixed)) o.mv_type_plane = plane(plane_mvtype_);
      } else if (pic_.type == kPictureB) {
        if (!pic_.skip_raw) o.skip_plane = plane(plane_skip_);
        if (!pic_.direct_raw) o.direct_plane = plane(plane_direct_);
      } else if (intra_like && adv) {
        if (!pic_.acpred_raw) o.acpred_plane = plane(plane_acpred_);
        if (!pic_.overflags_raw && seq_.overlap && pic_.pq <= 8 && pic_.condover == kCondOverSelect) o.overflags_plane = plane(plane_overflags_);
      }
    }
    o.ic_forward = ic_use_ && (pic_.type == kPictureP || pic_.type == kPictureB);
    o.ic_scale = ic_scale_;
    o.ic_shift = ic_shift_;
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

// ---- decoding for a DXVA driver --------------------------------------------------------------------------------------------------------

Core::ExternalStatus Core::decode_external(const SequenceHeader& seq, ConstByteView data, const ExternalPicture& picture, PictureHeader* header) {
  *header = PictureHeader();
  pic_ = PictureHeader();
  if (!picture.target || data.empty()) return ExternalStatus::BadPicture;
  offload_ = nullptr;
  observer_ = nullptr;
  seq_ = seq;
  seq_.valid = true;
  have_seq_ = true;
  if (!setup_sequence()) return ExternalStatus::BadSequence;
  Buf<uint8_t> scratch;
  if (!scratch.alloc(allocator_, data.size() + 8)) return ExternalStatus::NoMemory;
  List<Part> parts(allocator_);
  List<ConstByteView> units(allocator_);
  bool have_frame = false;
  uint64_t used = 0;
  const bool adv = seq_.profile == kAdvanced;
  auto stash = [&](ConstByteView unit) -> ConstByteView {
    const uint64_t n = unescape(unit, ByteView(scratch.data(), scratch.size()).subview(used));
    ConstByteView out = ConstByteView(scratch.data(), scratch.size()).subview(used, n);
    used += n;
    return out;
  };
  const bool coded = adv && data.size() >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 1;
  if (!coded) {
    Part frame;
    frame.code = kCodeFrame;
    frame.data = adv ? stash(data) : data;
    frame.raw = data;
    have_frame = true;
    if (!parts.push_back(frame) || !units.push_back(data)) return ExternalStatus::NoMemory;
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
      if (code == kCodeFrame || code == kCodeSlice) {
        if (code == kCodeFrame && have_frame) break;   // (a second frame: not this picture's)
        if (code == kCodeSlice && !have_frame) return ExternalStatus::BadPicture;
        have_frame = true;
        Part part;
        part.code = code;
        part.data = stash(unit);
        part.raw = whole;
        if (!parts.push_back(part) || !units.push_back(whole)) return ExternalStatus::NoMemory;
      } else if (code == kCodeField) {
        // The second field of a field interlace frame (DXVA sends it as a picture of its own).
        return ExternalStatus::Interlaced;
      }
      // (sequence headers, entry points, end of sequence and user data: the parameters are `seq`)
      i = end;
    }
  }
  if (!have_frame) return ExternalStatus::BadPicture;
  ext_ = &picture;
  const bool ok = decode_frame(parts.view(), units.view());
  ext_ = nullptr;
  anchor_[0] = anchor_[1] = nullptr;
  cur_ = nullptr;
  *header = pic_;
  if (!ok) return pic_.fcm != kFcmProgressive ? ExternalStatus::Interlaced : ExternalStatus::BadPicture;
  return ExternalStatus::Decoded;
}

}  // namespace vc1
}  // namespace hardware
