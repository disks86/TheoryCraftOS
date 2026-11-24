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

// H.264 decoding, part 2: slice data and macroblock syntax (CAVLC and CABAC), the macroblock addressing of field pictures and MBAFF frames with their
// neighbouring locations (6.4.12), motion vector prediction and the direct modes, residual parsing and dequantisation.

#include "Hardware/VideoCodec/Software/H264Core.hpp"

namespace hardware {
namespace h264 {

#include "Hardware/VideoCodec/Software/H264Tables.inc"

namespace {

inline int clip3(int lo, int hi, int v) { return v < lo ? lo : v > hi ? hi : v; }
inline int iabs(int v) { return v < 0 ? -v : v; }
inline int imin(int a, int b) { return a < b ? a : b; }
inline int imax(int a, int b) { return a > b ? a : b; }
inline int median(int a, int b, int c) { return imax(imin(a, b), imin(imax(a, b), c)); }

enum MbKind { kI4x4 = 0, kI8x8, kI16, kPcm, kInter };

// Index of 4x4 block (x, y) in decoding (z) order.
const uint8_t kZOrder[16] = {0, 1, 4, 5, 2, 3, 6, 7, 8, 9, 12, 13, 10, 11, 14, 15};
// 4x4 block number in decoding order -> raster position (x + 4 * y).
const uint8_t kBlkRaster[16] = {0, 1, 4, 5, 2, 3, 6, 7, 8, 9, 12, 13, 10, 11, 14, 15};

struct BTypeInfo {
  uint8_t part_mode;
  uint8_t pred0, pred1;
};
// B macroblock types 1..21: partitioning and the prediction of each partition (1 = list 0, 2 = list 1, 3 = both).
const BTypeInfo kBTypes[23] = {
    {0, 0, 0}, {0, 1, 0}, {0, 2, 0}, {0, 3, 0}, {1, 1, 1}, {2, 1, 1}, {1, 2, 2}, {2, 2, 2},
    {1, 1, 2}, {2, 1, 2}, {1, 2, 1}, {2, 2, 1}, {1, 1, 3}, {2, 1, 3}, {1, 2, 3}, {2, 2, 3},
    {1, 3, 1}, {2, 3, 1}, {1, 3, 2}, {2, 3, 2}, {1, 3, 3}, {2, 3, 3}, {3, 0, 0}};

// B sub-macroblock types: partition shape (0 8x8, 1 8x4, 2 4x8, 3 4x4) and prediction (0 = direct).
const uint8_t kBSubShape[13] = {0, 0, 0, 0, 1, 2, 1, 2, 1, 2, 3, 3, 3};
const uint8_t kBSubPred[13] = {0, 1, 2, 3, 1, 1, 2, 2, 3, 3, 1, 2, 3};

int poc_of(const RefPic& r) { return r.parity == 1 ? r.pic->top_poc : r.parity == 2 ? r.pic->bottom_poc : r.pic->poc; }
bool long_of(const RefPic& r) { return r.parity ? r.pic->field_ref[r.parity - 1] == 2 : r.pic->is_long; }
// The picture a reference index names, as MotionMb::refid keeps it.
int code_of(const RefPic& r) { return r.pic ? r.pic->id * 4 + (r.parity ? r.parity : 3) : -1; }

}  // namespace

// ---------------------------------------------------------------------------------------------------------------
// Macroblock addresses and neighbours

// The macroblock at address `addr` of the current picture becomes the current one (with the field decoding flag of its pair for an MBAFF frame: set_mb_field()).
void Core::setup_mb(int addr) {
  mb_addr_ = addr;
  if (mbaff_) {
    const int pair = addr >> 1;
    mbx_ = pair % mb_w_;
    mb_bottom_ = (addr & 1) != 0;
  } else {
    mbx_ = addr % mb_w_;
    mby_ = addr / mb_w_;
    mb_bottom_ = false;
  }
  cm_ = &mbs_[static_cast<uint64_t>(addr)];
  cmotion_ = &cur_->motion[static_cast<uint64_t>(motion_base_ + addr)];
  set_mb_field(mbaff_ ? mb_field_ : field_pic_);
}

// What depends on the current macroblock being a field or a frame macroblock: its samples (a field macroblock of an MBAFF frame takes every other line of its pair,
// 6.4.1), its position for motion compensation, its reference lists (8.4.2.1) and the inverse scans.
void Core::set_mb_field(bool field) {
  mb_field_ = field;
  if (mbaff_) {
    const int pair_row = (mb_addr_ >> 1) / mb_w_;
    if (field) {
      mby_ = pair_row;
      mb_y_ = pic_y_ + static_cast<int64_t>(32 * pair_row + (mb_bottom_ ? 1 : 0)) * pic_ls_ + 16 * mbx_;
      mb_u_ = pic_u_ + static_cast<int64_t>(16 * pair_row + (mb_bottom_ ? 1 : 0)) * pic_cs_ + 8 * mbx_;
      mb_v_ = pic_v_ + static_cast<int64_t>(16 * pair_row + (mb_bottom_ ? 1 : 0)) * pic_cs_ + 8 * mbx_;
      mb_ls_ = 2 * pic_ls_;
      mb_cs_ = 2 * pic_cs_;
      mc_y_ = 16 * pair_row;
      mc_h_ = mb_h_ * 8;
      rl_ = mb_bottom_ ? 2 : 1;
      mb_poc_ = mb_bottom_ ? dpb_.bottom_poc() : dpb_.top_poc();
    } else {
      mby_ = 2 * pair_row + (mb_bottom_ ? 1 : 0);
    }
  }
  if (!mbaff_ || !field) {
    mb_y_ = pic_y_ + static_cast<int64_t>(16 * mby_) * pic_ls_ + 16 * mbx_;
    mb_u_ = pic_u_ + static_cast<int64_t>(8 * mby_) * pic_cs_ + 8 * mbx_;
    mb_v_ = pic_v_ + static_cast<int64_t>(8 * mby_) * pic_cs_ + 8 * mbx_;
    mb_ls_ = pic_ls_;
    mb_cs_ = pic_cs_;
    mc_y_ = 16 * mby_;
    mc_h_ = pic_mb_h_ * 16;
    rl_ = 0;
    mb_poc_ = pic_poc_;
  }
  ref_count_[0] = ref_n_[rl_][0];
  ref_count_[1] = ref_n_[rl_][1];
  scan4_ = field ? kFieldScan4x4 : kZigzag4x4;
  scan8_ = field ? kFieldScan8x8 : kZigzag8x8;
}

// 7.4.4: mb_field_decoding_flag of a pair that sends none - that of the pair to the left in the same slice, else of the pair above, else frame.
bool Core::inferred_mb_field() const {
  const int pair = mb_addr_ >> 1;
  if (pair % mb_w_ > 0 && mbs_[static_cast<uint64_t>(2 * (pair - 1))].slice == slice_no_) return mbs_[static_cast<uint64_t>(2 * (pair - 1))].field != 0;
  if (pair >= mb_w_ && mbs_[static_cast<uint64_t>(2 * (pair - mb_w_))].slice == slice_no_) return mbs_[static_cast<uint64_t>(2 * (pair - mb_w_))].field != 0;
  return false;
}

// 6.4.12: the macroblock holding the luma (or, with `chroma`, the 4:2:0 chroma) location (xn, yn) relative to the current macroblock, and the location inside
// it; addr -1 when it is not available (outside the picture, not decoded, or in another slice).
NbLoc Core::neighbour(int xn, int yn, bool chroma) const {
  const int maxw = chroma ? 8 : 16, maxh = chroma ? 8 : 16;
  NbLoc out;
  if (yn > maxh - 1 || (xn > maxw - 1 && yn >= 0)) return out;
  if (!mbaff_) {
    // 6.4.12.1
    int addr;
    if (xn < 0) {
      if (mbx_ == 0) return out;
      addr = yn < 0 ? mb_addr_ - mb_w_ - 1 : mb_addr_ - 1;
    } else if (xn < maxw) {
      addr = yn < 0 ? mb_addr_ - mb_w_ : mb_addr_;
    } else {
      if (mbx_ == mb_w_ - 1) return out;
      addr = mb_addr_ - mb_w_ + 1;
    }
    if (addr < 0 || (addr != mb_addr_ && mbs_[static_cast<uint64_t>(addr)].slice != slice_no_)) return out;
    out.addr = addr;
    out.x = (xn + maxw) % maxw;
    out.y = (yn + maxh) % maxh;
    return out;
  }
  // 6.4.12.2, Table 6-4. The neighbouring pairs (6.4.10) by their top macroblocks.
  const int pair = mb_addr_ >> 1, px = pair % mb_w_;
  auto top_of = [&](int p) { return (p >= 0 && mbs_[static_cast<uint64_t>(2 * p)].slice == slice_no_) ? 2 * p : -1; };
  auto frame_pair = [&](int top) { return mbs_[static_cast<uint64_t>(top)].field == 0; };
  const bool cur_frame = !mb_field_, is_top = !mb_bottom_;
  int addr = -1, ym = yn;
  if (xn < 0 && yn < 0) {
    if (cur_frame && is_top) {
      const int d = top_of(px > 0 ? pair - mb_w_ - 1 : -1);
      if (d < 0) return out;
      addr = d + 1;
    } else if (cur_frame) {
      const int a = top_of(px > 0 ? pair - 1 : -1);
      if (a < 0) return out;
      if (frame_pair(a)) {
        addr = a;
      } else {
        addr = a + 1;
        ym = (yn + maxh) >> 1;
      }
    } else {
      const int d = top_of(px > 0 ? pair - mb_w_ - 1 : -1);
      if (d < 0) return out;
      if (!is_top) {
        addr = d + 1;
      } else if (frame_pair(d)) {
        addr = d + 1;
        ym = 2 * yn;
      } else {
        addr = d;
      }
    }
  } else if (xn < 0) {
    const int a = top_of(px > 0 ? pair - 1 : -1);
    if (a < 0) return out;
    const bool a_frame = frame_pair(a);
    if (cur_frame) {
      if (a_frame) {
        addr = is_top ? a : a + 1;
      } else {
        addr = a + (yn & 1);
        ym = is_top ? yn >> 1 : (yn + maxh) >> 1;
      }
    } else if (a_frame) {
      if (yn < maxh / 2) {
        addr = a;
        ym = is_top ? yn << 1 : (yn << 1) + 1;
      } else {
        addr = a + 1;
        ym = is_top ? (yn << 1) - maxh : (yn << 1) + 1 - maxh;
      }
    } else {
      addr = is_top ? a : a + 1;
    }
  } else if (xn < maxw) {
    if (yn >= 0) {
      addr = mb_addr_;
    } else if (cur_frame && !is_top) {
      addr = mb_addr_ - 1;   // the top macroblock of the pair
    } else {
      const int b = top_of(pair - mb_w_);
      if (b < 0) return out;
      if (cur_frame || !is_top) {
        addr = b + 1;
      } else if (frame_pair(b)) {
        addr = b + 1;
        ym = 2 * yn;
      } else {
        addr = b;
      }
    }
  } else {
    if (cur_frame && !is_top) return out;
    const int c = top_of(px < mb_w_ - 1 ? pair - mb_w_ + 1 : -1);
    if (c < 0) return out;
    if (cur_frame || !is_top) {
      addr = c + 1;
    } else if (frame_pair(c)) {
      addr = c + 1;
      ym = 2 * yn;
    } else {
      addr = c;
    }
  }
  out.addr = addr;
  out.x = (xn + maxw) % maxw;
  out.y = (ym + maxh) % maxh;
  return out;
}

// 6.4.11.1: the macroblocks A (left) and B (above).
const MbCtx* Core::nb_a() const { return nb_mb(neighbour(-1, 0, false)); }
const MbCtx* Core::nb_b() const { return nb_mb(neighbour(0, -1, false)); }

// Whether the samples at (xn, yn) can be used for intra prediction (8.3.1.2: available, and not Inter under constrained_intra_pred_flag).
bool Core::intra_ok(int xn, int yn, bool chroma) const {
  const MbCtx* m = nb_mb(neighbour(xn, yn, chroma));
  return m && (!pp_->constrained_intra_pred || m->intra);
}

// The same for a column of `rows` samples to the left (in an MBAFF frame they can come from both macroblocks of the pair to the left).
bool Core::intra_rows_ok(int xn, int y0, int rows, bool chroma) const {
  if (!mbaff_) return intra_ok(xn, y0, chroma);
  for (int y = y0; y < y0 + rows; ++y)
    if (!intra_ok(xn, y, chroma)) return false;
  return true;
}

// ---------------------------------------------------------------------------------------------------------------
// CABAC engine (9.3)

void Core::cabac_init_engine() {
  c_range_ = 510;
  c_offset_ = cbr_->u(9);
}

void Core::cabac_init_contexts() {
  const int model = sh_.type == kSliceI ? 0 : sh_.cabac_init_idc + 1;
  const int qp = clip3(0, 51, sh_.slice_qp);
  for (int i = 0; i < 460; ++i) {
    const int m = kCabacInit[(i * 4 + model) * 2], n = kCabacInit[(i * 4 + model) * 2 + 1];
    const int pre = clip3(1, 126, ((m * qp) >> 4) + n);
    if (pre <= 63) c_state_[i] = static_cast<uint8_t>((63 - pre) << 1);
    else c_state_[i] = static_cast<uint8_t>(((pre - 64) << 1) | 1);
  }
  for (int i = 460; i < 1024; ++i) c_state_[i] = 0;
}

int Core::cabac_decision(int ctx) {
  uint8_t& st = c_state_[ctx];
  int state = st >> 1, mps = st & 1;
  const uint32_t lps = kRangeLps[state * 4 + ((c_range_ >> 6) & 3)];
  c_range_ -= lps;
  int bin;
  if (c_offset_ >= c_range_) {
    bin = !mps;
    c_offset_ -= c_range_;
    c_range_ = lps;
    if (state == 0) mps = 1 - mps;
    state = kStateNext[state * 2];
  } else {
    bin = mps;
    state = kStateNext[state * 2 + 1];
  }
  st = static_cast<uint8_t>((state << 1) | mps);
  while (c_range_ < 256) {
    c_range_ <<= 1;
    c_offset_ = (c_offset_ << 1) | cbr_->bit();
  }
  return bin;
}

int Core::cabac_bypass() {
  c_offset_ = (c_offset_ << 1) | cbr_->bit();
  if (c_offset_ >= c_range_) {
    c_offset_ -= c_range_;
    return 1;
  }
  return 0;
}

int Core::cabac_terminate() {
  c_range_ -= 2;
  if (c_offset_ >= c_range_) return 1;
  while (c_range_ < 256) {
    c_range_ <<= 1;
    c_offset_ = (c_offset_ << 1) | cbr_->bit();
  }
  return 0;
}

// ---------------------------------------------------------------------------------------------------------------
// Slice data (7.3.4)

bool Core::decode_slice(BitReader& br, const SliceHeader& sh) {
  const Pps& pps = *pp_;
  ++slice_no_;
  qp_ = sh.slice_qp;
  prev_qp_delta_nz_ = 0;
  const int addr = mbaff_ ? 2 * sh.first_mb : sh.first_mb;   // CurrMbAddr = first_mb_in_slice * ( 1 + MbaffFrameFlag )
  cbr_ = &br;
  bottom_skip_known_ = false;
  mb_field_ = field_pic_;
  if (pps.cabac) {
    br.align();
    cabac_init_contexts();
    cabac_init_engine();
    return decode_slice_cabac(addr);
  }
  return decode_slice_cavlc(br, addr);
}

bool Core::decode_slice_cavlc(BitReader& br, int addr) {
  const int total = pic_mbs_;
  bool pair_field = false;    // MBAFF: mb_field_decoding_flag of the current pair
  bool field_read = false;    // and whether it was read already (with a skipped top macroblock)
  while (addr < total) {
    bool prev_skipped = false;
    if (sh_.type != kSliceI) {
      const uint32_t run = br.ue();
      if (br.error() || run > static_cast<uint32_t>(total - addr)) return false;
      for (uint32_t i = 0; i < run; ++i) {
        if (mbaff_ && !(addr & 1)) {
          mb_addr_ = addr;
          pair_field = inferred_mb_field();
          field_read = false;
          // The run ends with this top macroblock: the bottom one is not skipped and its mb_field_decoding_flag, the next bit, is the pair's.
          if (i + 1 == run && br.more_rbsp_data()) {
            pair_field = br.u(1) != 0;
            field_read = true;
          }
        }
        mb_field_ = pair_field;
        setup_mb(addr);
        if (!finish_mb(true)) return false;
        ++addr;
      }
      prev_skipped = run > 0;
      if (run > 0 && !br.more_rbsp_data()) break;
      if (addr >= total) break;
    }
    if (mbaff_) {
      if (!(addr & 1)) {
        pair_field = br.u(1) != 0;
        field_read = true;
      } else if (prev_skipped && !field_read) {
        pair_field = br.u(1) != 0;
        field_read = true;
      }
    }
    mb_field_ = pair_field;
    setup_mb(addr);
    if (!decode_mb_cavlc(br)) return false;
    ++addr;
    if (!br.more_rbsp_data() || br.error()) break;
  }
  return true;
}

bool Core::decode_slice_cabac(int addr) {
  const int total = pic_mbs_;
  bool pair_field = false;
  while (addr < total) {
    const bool is_top = !mbaff_ || !(addr & 1);
    if (mbaff_ && is_top) {
      mb_addr_ = addr;
      pair_field = inferred_mb_field();
      bottom_skip_known_ = false;
    }
    mb_field_ = pair_field;
    setup_mb(addr);
    bool skipped = false;
    if (sh_.type != kSliceI) {
      if (mbaff_ && !is_top && bottom_skip_known_) {
        skipped = bottom_skipped_;
      } else {
        skipped = cabac_mb_skip() != 0;
      }
      if (skipped && mbaff_ && is_top) {
        // 7.3.4: the bottom macroblock's mb_skip_flag follows (its contexts see the top one skipped, the pair's flag still inferred), and when it is not
        // skipped its mb_field_decoding_flag, which the top macroblock decodes with.
        cm_->skip = 1;
        cm_->slice = slice_no_;
        cm_->field = static_cast<uint8_t>(pair_field);
        cm_->intra = 0;
        setup_mb(addr + 1);
        bottom_skipped_ = cabac_mb_skip() != 0;
        bottom_skip_known_ = true;
        if (!bottom_skipped_) pair_field = cabac_mb_field() != 0;
        mb_field_ = pair_field;
        setup_mb(addr);
      }
      if (skipped && !finish_mb(true)) return false;
    }
    if (!skipped) {
      if (mbaff_ && is_top) {
        pair_field = cabac_mb_field() != 0;
        set_mb_field(pair_field);
      }
      if (!decode_mb_cabac()) return false;
    }
    ++addr;
    if (mbaff_ && is_top) continue;   // (no end_of_slice_flag after the top macroblock of a pair)
    if (cabac_terminate()) break;
    if (cbr_->error()) return false;
  }
  return true;
}

// Resets the per-macroblock state before a macroblock is parsed.
void Core::set_inter_defaults() {
  MbCtx* m = cm_;
  m->intra = m->skip = m->direct16 = m->t8x8 = m->i16 = m->pcm = 0;
  m->cbp = 0;
  m->chroma_mode = 0;
  m->nzmask = 0;
  for (int i = 0; i < 24; ++i) m->nz[i] = 0;
  m->cbf_dc = 0;
  m->direct_mask = 0;
  m->field = static_cast<uint8_t>(mb_field_);
  for (int i = 0; i < 16; ++i) m->ipred[i] = 2;
  for (int l = 0; l < 2; ++l)
    for (int i = 0; i < 16; ++i) m->mvd[l][i][0] = m->mvd[l][i][1] = 0;
  MotionMb* mo = cmotion_;
  mo->intra = 0;
  mo->field = static_cast<uint8_t>(mb_field_);
  for (int l = 0; l < 2; ++l) {
    for (int i = 0; i < 16; ++i) mo->mv[l][i][0] = mo->mv[l][i][1] = 0;
    for (int i = 0; i < 4; ++i) {
      mo->ref[l][i] = -1;
      mo->refid[l][i] = -1;
    }
  }
  done_mask_ = 0;
  rect_count_ = 0;
  cbp_ = 0;
  for (int p = 0; p < 3; ++p) {
    for (int i = 0; i < 256; ++i) coef_[p][i] = 0;
    for (int i = 0; i < 16; ++i) coef_present_[p][i] = false;
  }
  for (int i = 0; i < 16; ++i) dc_luma_[i] = 0;
}

void Core::update_qp() {
  cm_->qp = static_cast<int8_t>(qp_);
  for (int c = 0; c < 2; ++c) cm_->qpc[c] = static_cast<int8_t>(kChromaQp[clip3(0, 51, qp_ + pp_->chroma_qp_offset[c])]);
}

// Common end of every macroblock: reconstructs it and records what neighbours and the loop filter need.
bool Core::finish_mb(bool skipped) {
  if (skipped) {
    set_inter_defaults();
    cm_->skip = 1;
    mb_type_ = kInter;
    if (sh_.type == kSliceB) {
      cm_->direct16 = 1;
      cm_->direct_mask = 15;
      part_mode_ = 3;
      direct_block(15);
    } else {
      skip_motion();
    }
    prev_qp_delta_nz_ = 0;
  }
  if (mb_type_ == kPcm) {
    cm_->qp = 0;
    for (int c = 0; c < 2; ++c) cm_->qpc[c] = static_cast<int8_t>(kChromaQp[clip3(0, 51, pp_->chroma_qp_offset[c])]);
  } else {
    update_qp();
  }
  cm_->dbf_idc = static_cast<uint8_t>(sh_.disable_deblocking);
  cm_->alpha_off = static_cast<int8_t>(sh_.alpha_off);
  cm_->beta_off = static_cast<int8_t>(sh_.beta_off);
  cm_->intra = mb_type_ != kInter;
  cm_->field = static_cast<uint8_t>(mb_field_);
  cmotion_->intra = cm_->intra;
  cmotion_->field = static_cast<uint8_t>(mb_field_);
  if (cm_->intra) {
    for (int l = 0; l < 2; ++l)
      for (int i = 0; i < 4; ++i) {
        cmotion_->ref[l][i] = -1;
        cmotion_->refid[l][i] = -1;
      }
  }
  reconstruct_mb();
  cm_->slice = slice_no_;
  return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Macroblock layer (CAVLC)

bool Core::read_pcm(BitReader* br) {
  br->align();
  const uint64_t bytes = mono_ ? 256 : 384;
  if (br->bits_left() < bytes * 8) return false;
  for (int i = 0; i < 256; ++i) mb_y_[static_cast<int64_t>(i >> 4) * mb_ls_ + (i & 15)] = static_cast<uint8_t>(br->u(8));
  if (mono_) return true;   // (no chroma samples: the planes stay mid-grey)
  for (int c = 0; c < 2; ++c) {
    uint8_t* plane = c == 0 ? mb_u_ : mb_v_;
    for (int i = 0; i < 64; ++i) plane[static_cast<int64_t>(i >> 3) * mb_cs_ + (i & 7)] = static_cast<uint8_t>(br->u(8));
  }
  return true;
}

bool Core::decode_mb_cavlc(BitReader& br) {
  set_inter_defaults();
  MbCtx* m = cm_;
  const uint32_t t0 = br.ue();
  if (br.error()) return false;
  int itype = -1;  // intra mb_type 0..25, or -1 for inter
  if (sh_.type == kSliceI) {
    if (t0 > 25) return false;
    itype = static_cast<int>(t0);
  } else if (sh_.type == kSliceP) {
    if (t0 > 30) return false;
    if (t0 >= 5) itype = static_cast<int>(t0) - 5;
  } else {
    if (t0 > 48) return false;
    if (t0 >= 23) itype = static_cast<int>(t0) - 23;
  }
  bool no_sub_lt8 = true;
  if (itype >= 0) {
    if (itype == 25) {
      mb_type_ = kPcm;
      if (!read_pcm(&br)) return false;
      m->pcm = 1;
      m->cbp = 0x2f;
      m->cbf_dc = 7;
      for (int i = 0; i < 24; ++i) m->nz[i] = 16;
      m->nzmask = 0xffff;
      prev_qp_delta_nz_ = 0;
      return finish_mb(false);
    }
    if (itype == 0) {
      mb_type_ = kI4x4;
      if (pp_->transform_8x8) {
        m->t8x8 = static_cast<uint8_t>(br.u(1));
        if (m->t8x8) mb_type_ = kI8x8;
      }
      const int n = m->t8x8 ? 4 : 16;
      for (int i = 0; i < n; ++i) {
        const int flag = static_cast<int>(br.u(1));
        int rem = 0;
        if (!flag) rem = static_cast<int>(br.u(3));
        const int blk = m->t8x8 ? (i & 1) * 2 + (i >> 1) * 8 : kBlkRaster[i];
        const int bx = blk & 3, by = blk >> 2;
        const int pred = predicted_intra_mode(bx, by, m->t8x8 != 0);
        const int mode = flag ? pred : (rem < pred ? rem : rem + 1);
        if (m->t8x8) {
          for (int dy = 0; dy < 2; ++dy)
            for (int dx = 0; dx < 2; ++dx) m->ipred[(by + dy) * 4 + bx + dx] = static_cast<int8_t>(mode);
        } else {
          m->ipred[by * 4 + bx] = static_cast<int8_t>(mode);
        }
      }
    } else {
      mb_type_ = kI16;
      m->i16 = 1;
      i16_mode_ = (itype - 1) & 3;
      cbp_ = (((itype - 1) >> 2) % 3) << 4 | ((itype - 1) / 12 ? 15 : 0);
      if (mono_ && (cbp_ >> 4)) return false;   // (CodedBlockPatternChroma is 0 without chroma)
    }
    if (!mono_) {
      m->chroma_mode = static_cast<uint8_t>(br.ue());
      if (m->chroma_mode > 3) return false;
    }
  } else {
    mb_type_ = kInter;
    if (!parse_inter_cavlc_type(br, static_cast<int>(t0), &no_sub_lt8)) return false;
  }
  if (br.error()) return false;
  if (mb_type_ != kI16) {
    const uint32_t code = br.ue();
    if (code > (mono_ ? 15u : 47u)) return false;
    if (mono_) cbp_ = (mb_type_ == kInter) ? kCbpInterMono[code] : kCbpIntraMono[code];
    else cbp_ = (mb_type_ == kInter) ? kCbpInter[code] : kCbpIntra[code];
    if (mb_type_ == kInter && (cbp_ & 15) && pp_->transform_8x8 && no_sub_lt8 &&
        (!(cm_->direct16) || sp_->direct_8x8_inference)) {
      m->t8x8 = static_cast<uint8_t>(br.u(1));
    }
  }
  m->cbp = static_cast<uint8_t>(cbp_);
  if (cbp_ > 0 || mb_type_ == kI16) {
    const int dqp = br.se();
    if (dqp < -26 || dqp > 25) return false;
    qp_ = (qp_ + dqp + 52) % 52;
    prev_qp_delta_nz_ = dqp != 0;
    update_qp();
    if (!read_residual_cavlc(br)) return false;
  } else {
    prev_qp_delta_nz_ = 0;
  }
  return finish_mb(false);
}

// ---------------------------------------------------------------------------------------------------------------
// Intra prediction modes (8.3.1.1, 8.3.2.1)

int Core::predicted_intra_mode(int bx, int by, bool is8x8) const {
  int ma, mb;
  if (bx > 0) {
    ma = cm_->ipred[by * 4 + bx - 1];
  } else {
    const NbLoc n = neighbour(-1, 4 * by, false);
    const MbCtx* a = nb_mb(n);
    if (!a || (!a->intra && pp_->constrained_intra_pred)) return 2;
    int x4 = n.x >> 2, y4 = n.y >> 2;
    if (is8x8) {
      // The 4x4 block n = 1 of the neighbouring 8x8 block (its upper right one), or n = 3 for the lower left 8x8 block of a frame macroblock beside a field pair.
      x4 = 2 * (n.x >> 3) + 1;
      y4 = 2 * (n.y >> 3) + ((mbaff_ && !mb_field_ && a->field && by == 2) ? 1 : 0);
    }
    ma = a->ipred[y4 * 4 + x4];
  }
  if (by > 0) {
    mb = cm_->ipred[(by - 1) * 4 + bx];
  } else {
    const NbLoc n = neighbour(4 * bx, -1, false);
    const MbCtx* b = nb_mb(n);
    if (!b || (!b->intra && pp_->constrained_intra_pred)) return 2;
    int x4 = n.x >> 2, y4 = n.y >> 2;
    if (is8x8) {
      x4 = 2 * (n.x >> 3);   // n = 2: the lower left 4x4 block of the neighbouring 8x8 block
      y4 = 2 * (n.y >> 3) + 1;
    }
    mb = b->ipred[y4 * 4 + x4];
  }
  return imin(ma, mb);
}

// ---------------------------------------------------------------------------------------------------------------
// Inter macroblocks

// 8.4.1.3.2: the motion of the partition holding the luma location (xn, yn) relative to the current macroblock (6.4.11.7: the exact location matters in an
// MBAFF frame, where the lines of a pair to the left alternate between its macroblocks). *ref is -2 if the partition is not available, -1 if it does not use
// the list (or is intra). In an MBAFF frame a neighbour of the other kind (frame or field) has its vertical vector and reference index converted to the
// current macroblock's units.
void Core::fetch_motion(int list, int xn, int yn, int* ref, int16_t* mv) const {
  if (xn >= 0 && xn < 16 && yn >= 0 && yn < 16) {
    const int bx = xn >> 2, by = yn >> 2;
    if (!((done_mask_ >> (by * 4 + bx)) & 1)) {
      *ref = -2;
      mv[0] = mv[1] = 0;
      return;
    }
    *ref = cmotion_->ref[list][(by >> 1) * 2 + (bx >> 1)];
    mv[0] = cmotion_->mv[list][by * 4 + bx][0];
    mv[1] = cmotion_->mv[list][by * 4 + bx][1];
    return;
  }
  // Another macroblock.
  const NbLoc n = neighbour(xn, yn, false);
  const MbCtx* m = nb_mb(n);
  if (!m) {
    *ref = -2;
    mv[0] = mv[1] = 0;
    return;
  }
  if (m->intra) {
    *ref = -1;
    mv[0] = mv[1] = 0;
    return;
  }
  const MotionMb& mo = motion_of(n.addr);
  int r = mo.ref[list][(n.y >> 3) * 2 + (n.x >> 3)];
  int vx = mo.mv[list][(n.y >> 2) * 4 + (n.x >> 2)][0];
  int vy = mo.mv[list][(n.y >> 2) * 4 + (n.x >> 2)][1];
  if (r >= 0 && mbaff_ && (m->field != 0) != mb_field_) {
    if (mb_field_) {
      vy /= 2;   // (8-217: "/" truncates)
      r *= 2;
    } else {
      vy *= 2;
      r >>= 1;
    }
  }
  *ref = r;
  mv[0] = static_cast<int16_t>(vx);
  mv[1] = static_cast<int16_t>(vy);
}

// 8.4.1.3: the predicted motion vector of the partition at 4x4 position (bx, by), w x h 4x4 blocks.
// shape: 0 = generic, 1 = 16x8 partition, 2 = 8x16 partition (with `part` the partition index).
void Core::predict_mv(int list, int bx, int by, int w, int h, int ref, int shape, int part, int* mvp) {
  (void)h;
  int ra, rb, rc;
  int16_t ma[2], mb[2], mc[2];
  fetch_motion(list, 4 * bx - 1, 4 * by, &ra, ma);
  fetch_motion(list, 4 * bx, 4 * by - 1, &rb, mb);
  fetch_motion(list, 4 * (bx + w), 4 * by - 1, &rc, mc);
  if (rc == -2) fetch_motion(list, 4 * bx - 1, 4 * by - 1, &rc, mc);
  if (shape == 1) {
    if (part == 0 && rb == ref) { mvp[0] = mb[0]; mvp[1] = mb[1]; return; }
    if (part == 1 && ra == ref) { mvp[0] = ma[0]; mvp[1] = ma[1]; return; }
  } else if (shape == 2) {
    if (part == 0 && ra == ref) { mvp[0] = ma[0]; mvp[1] = ma[1]; return; }
    if (part == 1 && rc == ref) { mvp[0] = mc[0]; mvp[1] = mc[1]; return; }
  }
  if (rb == -2 && rc == -2 && ra != -2) {
    rb = rc = ra;
    mb[0] = mc[0] = ma[0];
    mb[1] = mc[1] = ma[1];
  }
  const int match = (ra == ref) + (rb == ref) + (rc == ref);
  if (match == 1) {
    const int16_t* s = ra == ref ? ma : rb == ref ? mb : mc;
    mvp[0] = s[0];
    mvp[1] = s[1];
    return;
  }
  // Unavailable and unused neighbours count as zero vectors.
  if (ra < 0) ma[0] = ma[1] = 0;
  if (rb < 0) mb[0] = mb[1] = 0;
  if (rc < 0) mc[0] = mc[1] = 0;
  mvp[0] = median(ma[0], mb[0], mc[0]);
  mvp[1] = median(ma[1], mb[1], mc[1]);
}

void Core::set_motion(int list, int bx, int by, int w, int h, int ref, int mvx, int mvy) {
  const int id = (ref >= 0 && ref < ref_count_[list]) ? code_of(ref_pic(list, ref)) : -1;
  for (int y = by; y < by + h; ++y) {
    for (int x = bx; x < bx + w; ++x) {
      cmotion_->mv[list][y * 4 + x][0] = static_cast<int16_t>(mvx);
      cmotion_->mv[list][y * 4 + x][1] = static_cast<int16_t>(mvy);
      cmotion_->ref[list][(y >> 1) * 2 + (x >> 1)] = static_cast<int8_t>(ref);
      cmotion_->refid[list][(y >> 1) * 2 + (x >> 1)] = id;
      done_mask_ = static_cast<uint16_t>(done_mask_ | (1u << (y * 4 + x)));
    }
  }
}

void Core::set_mvd(int list, int bx, int by, int w, int h, int dx, int dy) {
  const int ax = imin(iabs(dx), 255), ay = imin(iabs(dy), 255);
  for (int y = by; y < by + h; ++y)
    for (int x = bx; x < bx + w; ++x) {
      cm_->mvd[list][y * 4 + x][0] = static_cast<uint8_t>(ax);
      cm_->mvd[list][y * 4 + x][1] = static_cast<uint8_t>(ay);
    }
}

void Core::skip_motion() {
  // P_Skip (8.4.1.1): reference 0 and the predicted vector, or zero.
  set_motion(1, 0, 0, 4, 4, -1, 0, 0);
  done_mask_ = 0;
  int ra, rb;
  int16_t ma[2], mb[2];
  fetch_motion(0, -1, 0, &ra, ma);
  fetch_motion(0, 0, -1, &rb, mb);
  int mv[2] = {0, 0};
  if (ra == -2 || rb == -2 || (ra == 0 && ma[0] == 0 && ma[1] == 0) || (rb == 0 && mb[0] == 0 && mb[1] == 0)) {
    // zero
  } else {
    predict_mv(0, 0, 0, 4, 4, 0, 0, 0, mv);
  }
  set_motion(0, 0, 0, 4, 4, 0, mv[0], mv[1]);
  rect_[0][0] = 0; rect_[0][1] = 0; rect_[0][2] = 4; rect_[0][3] = 4;
  rect_count_ = 1;
}

// 8.4.1.2.1: the co-located 4x4 block of the current macroblock's block (bx, by) - colPic (Table 8-6), mbAddrCol, yM and vertMvScale (Table 8-8) - and its
// motion: the list 0 motion, or the list 1 motion when list 0 is unused.
void Core::col_motion(int bx, int by, Colocated* out) const {
  *out = Colocated{};
  const RefPic& first = refs_[0][1][0];   // RefPicList1[ 0 ] (a frame, or for a field picture a field)
  const Picture* col = first.pic;
  if (!col || col->non_existing) return;
  const int w = mb_w_;
  const int field_mbs = w * (mb_h_ / 2);
  const int xcol = 4 * bx, ycol = 4 * by;
  const int cur = mb_addr_;
  auto abs_diff = [&](int poc) { return iabs(poc - pic_poc_); };
  int col_addr = cur, ym = ycol, base = 0;
  if (field_pic_) {
    if (col->field_coded) {
      base = first.parity == 2 ? field_mbs : 0;   // the field RefPicList1[ 0 ]
    } else if (!col->mbaff) {
      col_addr = 2 * w * (cur / w) + (cur % w) + w * (ycol / 8);   // mbAddrCol1, in the frame containing it
      ym = (2 * ycol) % 16;
      out->scale = 1;
    } else {
      const int x = 2 * cur;
      if (!col->motion[static_cast<uint64_t>(x)].field) {
        col_addr = 2 * cur + (ycol / 8);   // mbAddrCol2
        ym = (2 * ycol) % 16;
        out->scale = 1;
      } else {
        col_addr = 2 * cur + (structure_ == 2 ? 1 : 0);   // mbAddrCol3
      }
    }
  } else if (col->field_coded) {
    // A frame whose co-located picture is a complementary field pair: one of its fields.
    bool bottom;
    if (mbaff_ && mb_field_) bottom = mb_bottom_;
    else bottom = !(abs_diff(col->top_poc) < abs_diff(col->bottom_poc));
    base = bottom ? field_mbs : 0;
    if (!mbaff_) {
      col_addr = w * (cur / (2 * w)) + (cur % w);   // mbAddrCol4
      ym = 8 * ((cur / w) % 2) + 4 * (ycol / 8);
      out->scale = 2;
    } else {
      col_addr = cur / 2;   // mbAddrCol5
      if (!mb_field_) {
        ym = 8 * (cur % 2) + 4 * (ycol / 8);
        out->scale = 2;
      }
    }
  } else if (mbaff_ == col->mbaff) {
    if (mbaff_) {
      const bool col_field = col->motion[static_cast<uint64_t>(cur)].field != 0;
      if (!mb_field_ && col_field) {
        col_addr = 2 * (cur / 2) + (abs_diff(col->top_poc) < abs_diff(col->bottom_poc) ? 0 : 1);   // mbAddrCol6
        ym = 8 * (cur % 2) + 4 * (ycol / 8);
        out->scale = 2;
      } else if (mb_field_ && !col_field) {
        col_addr = 2 * (cur / 2) + (ycol / 8);   // mbAddrCol7
        ym = (2 * ycol) % 16;
        out->scale = 1;
      }
    }
  } else {
    // (an MBAFF frame and a non-MBAFF one are never co-located without an IDR picture between them - NOTE of Table 8-8; the macroblock at the same place is used)
    const int px = mbaff_ ? (cur >> 1) % w : cur % w;
    const int py = mbaff_ ? 2 * ((cur >> 1) / w) + (cur & 1) : cur / w;
    col_addr = col->mbaff ? 2 * ((py >> 1) * w + px) + (py & 1) : py * w + px;
  }
  const uint64_t index = static_cast<uint64_t>(base + col_addr);
  if (index >= col->motion.size()) return;
  const MotionMb& mo = col->motion[index];
  if (mo.intra) return;
  const int b8 = (ym >> 3) * 2 + (xcol >> 3);
  const int list = mo.ref[0][b8] >= 0 ? 0 : 1;
  if (mo.ref[list][b8] < 0) return;
  out->intra = false;
  out->refidx = mo.ref[list][b8];
  out->refid = mo.refid[list][b8];
  out->mv[0] = mo.mv[list][(ym >> 2) * 4 + (xcol >> 2)][0];
  out->mv[1] = mo.mv[list][(ym >> 2) * 4 + (xcol >> 2)][1];
  out->field_mb = mo.field != 0;
}

// 8.4.1.2.3: MapColToList0 - the lowest index of the current list 0 that references the picture (or the frame or field of it) the co-located block referenced.
int Core::map_col_to_list0(const Colocated& col) const {
  if (col.refid < 0) return 0;
  const int id = col.refid >> 2, st = col.refid & 3;   // (st: 1 top field, 2 bottom field, 3 frame)
  const int n = ref_count_[0];
  if (col.scale == 0) {
    if (!field_pic_ && mb_field_) {
      // A field macroblock of a frame: the frame holding the field, then the field of the same (2i) or the other (2i + 1) parity as the macroblock.
      for (int i = 0; i < ref_n_[0][0]; ++i)
        if (refs_[0][0][i].pic && refs_[0][0][i].pic->id == id) return (i << 1) + (st == (mb_bottom_ ? 2 : 1) ? 0 : 1);
      return 0;
    }
    for (int i = 0; i < n; ++i)
      if (code_of(ref_pic(0, i)) == col.refid) return i;
    return 0;
  }
  if (col.scale == 1) {
    // Frm_To_Fld: the co-located frame macroblock referenced a frame.
    if (!field_pic_) {
      for (int i = 0; i < ref_n_[0][0]; ++i)
        if (refs_[0][0][i].pic && refs_[0][0][i].pic->id == id) return i << 1;
      return 0;
    }
    for (int i = 0; i < n; ++i) {
      const RefPic& r = ref_pic(0, i);
      if (r.pic && r.pic->id == id && r.parity == structure_) return i;
    }
    return 0;
  }
  // Fld_To_Frm: the frame (or field pair) holding the field referenced.
  for (int i = 0; i < n; ++i) {
    const RefPic& r = ref_pic(0, i);
    if (r.pic && r.pic->id == id) return i;
  }
  return 0;
}

void Core::direct_spatial(int mask8) {
  // Reference indices and the predicted vectors come from the neighbours of the whole macroblock.
  int ref[2];
  int mvp[2][2] = {{0, 0}, {0, 0}};
  uint16_t saved_done = done_mask_;
  done_mask_ = 0;
  for (int l = 0; l < 2; ++l) {
    int ra, rb, rc;
    int16_t ma[2], mb[2], mc[2];
    fetch_motion(l, -1, 0, &ra, ma);
    fetch_motion(l, 0, -1, &rb, mb);
    fetch_motion(l, 16, -1, &rc, mc);
    if (rc == -2) fetch_motion(l, -1, -1, &rc, mc);
    auto min_pos = [](int x, int y) { return (x >= 0 && y >= 0) ? imin(x, y) : imax(x, y); };
    ref[l] = min_pos(ra, min_pos(rb, rc));
    if (ref[l] < 0) ref[l] = -1;
  }
  bool zero_pred = false;
  if (ref[0] < 0 && ref[1] < 0) {
    ref[0] = ref[1] = 0;
    zero_pred = true;
  } else {
    for (int l = 0; l < 2; ++l)
      if (ref[l] >= 0) predict_mv(l, 0, 0, 4, 4, ref[l], 0, 0, mvp[l]);
  }
  done_mask_ = saved_done;
  const bool inference = sp_->direct_8x8_inference;
  const bool l1_short = ref_count_[1] > 0 && ref_pic(1, 0).pic && !long_of(ref_pic(1, 0));
  for (int q = 0; q < 4; ++q) {
    if (!((mask8 >> q) & 1)) continue;
    const int qx = (q & 1) * 2, qy = (q >> 1) * 2;
    const int step = inference ? 2 : 1;
    for (int sy = 0; sy < 2; sy += step) {
      for (int sx = 0; sx < 2; sx += step) {
        const int bx = qx + sx, by = qy + sy;
        const int cx = inference ? (qx ? 3 : 0) : bx;
        const int cy = inference ? (qy ? 3 : 0) : by;
        bool col_zero = false;
        if (l1_short && !zero_pred) {
          Colocated col;
          col_motion(cx, cy, &col);
          // (mvCol in the co-located macroblock's own units: NOTE 2 of 8.4.1.2.2)
          col_zero = !col.intra && col.refidx == 0 && col.mv[0] >= -1 && col.mv[0] <= 1 && col.mv[1] >= -1 && col.mv[1] <= 1;
        }
        for (int l = 0; l < 2; ++l) {
          int r = zero_pred ? 0 : ref[l];
          int mx = 0, my = 0;
          if (r >= 0 && !zero_pred && !(r == 0 && col_zero)) {
            mx = mvp[l][0];
            my = mvp[l][1];
          }
          set_motion(l, bx, by, step, step, r, mx, my);
        }
        if (inference) {
          rect_[rect_count_][0] = static_cast<uint8_t>(qx); rect_[rect_count_][1] = static_cast<uint8_t>(qy);
          rect_[rect_count_][2] = 2; rect_[rect_count_][3] = 2;
          ++rect_count_;
        } else {
          rect_[rect_count_][0] = static_cast<uint8_t>(bx); rect_[rect_count_][1] = static_cast<uint8_t>(by);
          rect_[rect_count_][2] = 1; rect_[rect_count_][3] = 1;
          ++rect_count_;
        }
      }
    }
  }
}

void Core::direct_temporal(int mask8) {
  const bool inference = sp_->direct_8x8_inference;
  for (int q = 0; q < 4; ++q) {
    if (!((mask8 >> q) & 1)) continue;
    const int qx = (q & 1) * 2, qy = (q >> 1) * 2;
    const int step = inference ? 2 : 1;
    for (int sy = 0; sy < 2; sy += step) {
      for (int sx = 0; sx < 2; sx += step) {
        const int bx = qx + sx, by = qy + sy;
        const int cx = inference ? (qx ? 3 : 0) : bx;
        const int cy = inference ? (qy ? 3 : 0) : by;
        Colocated col;
        col_motion(cx, cy, &col);
        int r0 = 0;
        int cmv[2] = {col.mv[0], col.mv[1]};
        if (!col.intra) {
          r0 = map_col_to_list0(col);
          if (r0 >= ref_count_[0]) r0 = 0;
          if (col.scale == 1) cmv[1] /= 2;        // Frm_To_Fld (8-193)
          else if (col.scale == 2) cmv[1] *= 2;   // Fld_To_Frm (8-194)
        }
        int mv0[2] = {0, 0}, mv1[2] = {0, 0};
        if (!col.intra) {
          const RefPic& p0 = ref_pic(0, r0);
          const RefPic& p1 = ref_pic(1, 0);
          const int poc0 = poc_of(p0), poc1 = poc_of(p1);
          if (long_of(p0) || poc1 == poc0) {
            mv0[0] = cmv[0];
            mv0[1] = cmv[1];
          } else {
            const int td = clip3(-128, 127, poc1 - poc0);
            const int tb = clip3(-128, 127, mb_poc_ - poc0);
            const int tx = (16384 + iabs(td / 2)) / td;
            const int dsf = clip3(-1024, 1023, (tb * tx + 32) >> 6);
            mv0[0] = (dsf * cmv[0] + 128) >> 8;
            mv0[1] = (dsf * cmv[1] + 128) >> 8;
            mv1[0] = mv0[0] - cmv[0];
            mv1[1] = mv0[1] - cmv[1];
          }
        }
        set_motion(0, bx, by, step, step, r0, mv0[0], mv0[1]);
        set_motion(1, bx, by, step, step, 0, mv1[0], mv1[1]);
        if (inference) {
          rect_[rect_count_][0] = static_cast<uint8_t>(qx); rect_[rect_count_][1] = static_cast<uint8_t>(qy);
          rect_[rect_count_][2] = 2; rect_[rect_count_][3] = 2;
          ++rect_count_;
        } else {
          rect_[rect_count_][0] = static_cast<uint8_t>(bx); rect_[rect_count_][1] = static_cast<uint8_t>(by);
          rect_[rect_count_][2] = 1; rect_[rect_count_][3] = 1;
          ++rect_count_;
        }
      }
    }
  }
}

void Core::direct_block(int mask8) {
  if (sh_.direct_spatial_mv_pred) direct_spatial(mask8);
  else direct_temporal(mask8);
}

// CAVLC te(v)
static int read_te(BitReader& br, int range) {
  if (range == 1) return br.u(1) ? 0 : 1;
  return static_cast<int>(br.ue());
}

// Parses an inter macroblock's type and prediction (7.3.5.1 / 7.3.5.2). `*no_sub_lt8` reports that no sub-macroblock
// is split below 8x8 (the condition for transform_size_8x8_flag).
bool Core::parse_inter_cavlc_type(BitReader& br, int t0, bool* no_sub_lt8) {
  const bool is_b = sh_.type == kSliceB;
  int part_mode, pred0 = 1, pred1 = 1;
  bool ref0 = false;
  if (!is_b) {
    if (t0 > 4) return false;
    part_mode = t0 == 0 ? 0 : t0 == 1 ? 1 : t0 == 2 ? 2 : 3;
    ref0 = t0 == 4;
  } else {
    if (t0 == 0) {  // B_Direct_16x16
      cm_->direct16 = 1;
      cm_->direct_mask = 15;
      part_mode_ = 3;
      direct_block(15);
      *no_sub_lt8 = true;
      return true;
    }
    part_mode = kBTypes[t0].part_mode;
    pred0 = kBTypes[t0].pred0;
    pred1 = kBTypes[t0].pred1;
  }
  part_mode_ = part_mode;
  int nparts = part_mode == 0 ? 1 : 2;
  int preds[4] = {pred0, pred1, 0, 0};
  int sub_shape[4] = {0, 0, 0, 0};
  *no_sub_lt8 = true;
  if (part_mode == 3) {
    nparts = 4;
    for (int i = 0; i < 4; ++i) {
      const uint32_t st = br.ue();
      if (!is_b) {
        if (st > 3) return false;
        sub_shape[i] = static_cast<int>(st);
        preds[i] = 1;
      } else {
        if (st > 12) return false;
        sub_shape[i] = kBSubShape[st];
        preds[i] = kBSubPred[st];
        if (st == 0) cm_->direct_mask = static_cast<uint8_t>(cm_->direct_mask | (1 << i));
      }
      if (preds[i] == 0) {
        if (!sp_->direct_8x8_inference) *no_sub_lt8 = false;
      } else if (sub_shape[i] != 0) {
        *no_sub_lt8 = false;
      }
    }
  }
  const int lists = is_b ? 2 : 1;
  // Reference indices.
  int refs[2][4] = {{-1, -1, -1, -1}, {-1, -1, -1, -1}};
  for (int l = 0; l < lists; ++l) {
    for (int p = 0; p < nparts; ++p) {
      if (!((preds[p] >> l) & 1)) continue;
      int r = 0;
      if (ref_count_[l] > 1 && !ref0) r = read_te(br, ref_count_[l] - 1);
      if (r < 0 || r >= ref_count_[l] || br.error()) return false;
      refs[l][p] = r;
    }
  }
  return finish_inter_parse(&br, part_mode, preds, sub_shape, refs);
}

// Reads the motion vector differences and sets up the motion of an inter macroblock (CAVLC and CABAC).
bool Core::finish_inter_parse(BitReader* br, int part_mode, const int* preds, const int* sub_shape, int refs[2][4]) {
  const bool is_b = sh_.type == kSliceB;
  const int lists = is_b ? 2 : 1;
  const bool cabac = pp_->cabac;
  rect_count_ = 0;
  const int nparts = part_mode == 0 ? 1 : part_mode == 3 ? 4 : 2;
  // The reference indices of all partitions are known now; store them so neighbouring partitions see them. (For
  // CABAC they were stored while parsing; the vectors follow.)
  for (int l = 0; l < lists; ++l) {
    done_mask_ = 0;
    for (int p = 0; p < nparts; ++p) {
      int bx, by, w, h;
      if (part_mode == 0) { bx = by = 0; w = h = 4; }
      else if (part_mode == 1) { bx = 0; by = p * 2; w = 4; h = 2; }
      else if (part_mode == 2) { bx = p * 2; by = 0; w = 2; h = 4; }
      else { bx = (p & 1) * 2; by = (p >> 1) * 2; w = h = 2; }
      if (part_mode == 3 && preds[p] == 0) {
        if (l == 0) direct_block(1 << p);
        else {
          // the list 0 pass computed both lists; mark the block as done for list 1
        }
        for (int y = by; y < by + 2; ++y)
          for (int x = bx; x < bx + 2; ++x) done_mask_ = static_cast<uint16_t>(done_mask_ | (1u << (y * 4 + x)));
        continue;
      }
      const bool uses = (preds[p] >> l) & 1;
      if (!uses) {
        set_motion(l, bx, by, w, h, -1, 0, 0);
        if (l == 0 && part_mode != 3) {
          // rect recorded below
        }
        continue;
      }
      const int ref = refs[l][p];
      // sub-partitions
      int nsub = 1, sw = w, sh = h;
      if (part_mode == 3) {
        switch (sub_shape[p]) {
          case 0: nsub = 1; sw = 2; sh = 2; break;
          case 1: nsub = 2; sw = 2; sh = 1; break;
          case 2: nsub = 2; sw = 1; sh = 2; break;
          default: nsub = 4; sw = 1; sh = 1; break;
        }
      }
      for (int s = 0; s < nsub; ++s) {
        int sx = bx, sy = by;
        if (part_mode == 3) {
          if (sub_shape[p] == 1) sy += s;
          else if (sub_shape[p] == 2) sx += s;
          else if (sub_shape[p] == 3) { sx += s & 1; sy += s >> 1; }
        }
        int mvp[2];
        const int shape = part_mode == 1 ? 1 : part_mode == 2 ? 2 : 0;
        predict_mv(l, sx, sy, sw, sh, ref, shape, p, mvp);
        int dx, dy;
        if (cabac) {
          // ref_idx must be in place for the mvd contexts of later partitions; mvd contexts use neighbours' mvd.
          dx = cabac_mvd(l, 0, sx, sy);
          dy = cabac_mvd(l, 1, sx, sy);
        } else {
          dx = br->se();
          dy = br->se();
        }
        set_motion(l, sx, sy, sw, sh, ref, mvp[0] + dx, mvp[1] + dy);
        set_mvd(l, sx, sy, sw, sh, dx, dy);
      }
    }
  }
  // Build the rectangle list for motion compensation (direct blocks added their own).
  for (int p = 0; p < nparts; ++p) {
    int bx, by, w, h;
    if (part_mode == 0) { bx = by = 0; w = h = 4; }
    else if (part_mode == 1) { bx = 0; by = p * 2; w = 4; h = 2; }
    else if (part_mode == 2) { bx = p * 2; by = 0; w = 2; h = 4; }
    else { bx = (p & 1) * 2; by = (p >> 1) * 2; w = h = 2; }
    if (part_mode == 3 && preds[p] == 0) continue;  // direct blocks added their own rectangles
    if (part_mode == 3) {
      switch (sub_shape[p]) {
        case 0: add_rect(bx, by, 2, 2); break;
        case 1: add_rect(bx, by, 2, 1); add_rect(bx, by + 1, 2, 1); break;
        case 2: add_rect(bx, by, 1, 2); add_rect(bx + 1, by, 1, 2); break;
        default:
          for (int s = 0; s < 4; ++s) add_rect(bx + (s & 1), by + (s >> 1), 1, 1);
          break;
      }
    } else {
      add_rect(bx, by, w, h);
    }
  }
  if (br && br->error()) return false;
  return true;
}

void Core::add_rect(int x, int y, int w, int h) {
  rect_[rect_count_][0] = static_cast<uint8_t>(x);
  rect_[rect_count_][1] = static_cast<uint8_t>(y);
  rect_[rect_count_][2] = static_cast<uint8_t>(w);
  rect_[rect_count_][3] = static_cast<uint8_t>(h);
  ++rect_count_;
}

// ---------------------------------------------------------------------------------------------------------------
// CAVLC residual

// nC for a block (9.2.1): plane 0 luma (bx, by in 0..3), plane 1/2 chroma (bx, by in 0..1); the neighbouring blocks A and B are those of 6.4.11.4 / 6.4.11.5.
int Core::cavlc_nc(int bx, int by, int plane) const {
  const bool chroma = plane != 0;
  const int w = chroma ? 2 : 4;
  const int base = chroma ? 16 + (plane - 1) * 4 : 0;
  int na = -1, nbv = -1;
  if (bx > 0) {
    na = cm_->nz[base + by * w + bx - 1];
  } else {
    const NbLoc n = neighbour(-1, 4 * by, chroma);
    if (const MbCtx* a = nb_mb(n)) na = a->nz[base + (n.y >> 2) * w + (n.x >> 2)];
  }
  if (by > 0) {
    nbv = cm_->nz[base + (by - 1) * w + bx];
  } else {
    const NbLoc n = neighbour(4 * bx, -1, chroma);
    if (const MbCtx* b = nb_mb(n)) nbv = b->nz[base + (n.y >> 2) * w + (n.x >> 2)];
  }
  if (na >= 0 && nbv >= 0) return (na + nbv + 1) >> 1;
  if (na >= 0) return na;
  if (nbv >= 0) return nbv;
  return 0;
}

// 9.2: one residual block. lvl[0 .. max_coeff-1] receives the levels in scan order; returns the number of non-zero
// coefficients or -1.
int Core::cavlc_residual(BitReader& br, int nc, int max_coeff, int* lvl) {
  for (int i = 0; i < max_coeff; ++i) lvl[i] = 0;
  const VlcTree& ct = nc < 0 ? coeff_token_chroma_dc_ : nc < 2 ? coeff_token_[0] : nc < 4 ? coeff_token_[1] : nc < 8 ? coeff_token_[2] : coeff_token_[3];
  const int sym = ct.decode(br);
  if (sym < 0) return -1;
  const int total = sym >> 2, t1 = sym & 3;
  if (total > max_coeff) return -1;
  if (total == 0) return 0;
  int level[16];
  int suffix_len = (total > 10 && t1 < 3) ? 1 : 0;
  for (int i = 0; i < total; ++i) {
    if (i < t1) {
      level[i] = 1 - 2 * static_cast<int>(br.bit());
      continue;
    }
    int prefix = 0;
    while (prefix < 32 && !br.bit()) {
      ++prefix;
      if (br.error()) return -1;
    }
    if (prefix > 25) return -1;
    int code = imin(15, prefix) << suffix_len;
    if (suffix_len > 0 || prefix >= 14) {
      int size = (prefix == 14 && suffix_len == 0) ? 4 : prefix >= 15 ? prefix - 3 : suffix_len;
      if (size > 0) code += static_cast<int>(br.u(size));
    }
    if (prefix >= 15 && suffix_len == 0) code += 15;
    if (prefix >= 16) code += (1 << (prefix - 3)) - 4096;
    if (i == t1 && t1 < 3) code += 2;
    level[i] = (code & 1) ? (-code - 1) >> 1 : (code + 2) >> 1;
    if (suffix_len == 0) suffix_len = 1;
    if (iabs(level[i]) > (3 << (suffix_len - 1)) && suffix_len < 6) ++suffix_len;
  }
  int zeros_left = 0;
  if (total < max_coeff) {
    const VlcTree& tz = nc < 0 ? total_zeros_chroma_dc_[total - 1] : total_zeros_[total - 1];
    zeros_left = tz.decode(br);
    if (zeros_left < 0) return -1;
  }
  if (total + zeros_left > max_coeff) return -1;
  int run[16];
  for (int i = 0; i < total - 1; ++i) {
    if (zeros_left > 0) {
      const int r = run_before_[imin(zeros_left, 7) - 1].decode(br);
      if (r < 0 || r > zeros_left) return -1;
      run[i] = r;
      zeros_left -= r;
    } else {
      run[i] = 0;
    }
  }
  run[total - 1] = zeros_left;
  int pos = -1;
  for (int i = total - 1; i >= 0; --i) {
    pos += run[i] + 1;
    if (pos >= max_coeff) return -1;
    lvl[pos] = level[i];
  }
  return total;
}

// Dequantises and stores a 4x4 block of levels (scan order list `lvl`, `count` entries from scan index `start`).
void Core::store4x4(int32_t* dst, const int* lvl, int start, int count, int list, int qp) {
  const int qpm = qp % 6, qpd = qp / 6;
  const int32_t* ls = level_scale4_[list][qpm];
  for (int i = 0; i < count; ++i) {
    if (!lvl[i]) continue;
    const int pos = scan4_[i + start];
    const int64_t v = static_cast<int64_t>(lvl[i]) * ls[pos];
    dst[pos] = static_cast<int32_t>(qpd >= 4 ? v << (qpd - 4) : (v + (1 << (3 - qpd))) >> (4 - qpd));
  }
}

void Core::store8x8(int32_t* dst, const int* lvl, int list, int qp) {
  const int qpm = qp % 6, qpd = qp / 6;
  const int32_t* ls = level_scale8_[list][qpm];
  for (int i = 0; i < 64; ++i) {
    if (!lvl[i]) continue;
    const int pos = scan8_[i];
    const int64_t v = static_cast<int64_t>(lvl[i]) * ls[pos];
    dst[pos] = static_cast<int32_t>(qpd >= 6 ? v << (qpd - 6) : (v + (1 << (5 - qpd))) >> (6 - qpd));
  }
}

// Inverse 4x4 Hadamard for the Intra16x16 DC and dequantisation (8.5.10); output goes to dc_luma_ in block raster order.
void Core::finish_luma_dc(const int* lvl) {
  int c[16];
  for (int i = 0; i < 16; ++i) c[i] = 0;
  for (int i = 0; i < 16; ++i) c[scan4_[i]] = lvl[i];
  int t[16];
  for (int i = 0; i < 4; ++i) {  // rows
    const int a = c[i * 4], b = c[i * 4 + 1], cc = c[i * 4 + 2], d = c[i * 4 + 3];
    t[i * 4 + 0] = a + b + cc + d;
    t[i * 4 + 1] = a + b - cc - d;
    t[i * 4 + 2] = a - b - cc + d;
    t[i * 4 + 3] = a - b + cc - d;
  }
  int f[16];
  for (int j = 0; j < 4; ++j) {  // columns
    const int a = t[j], b = t[4 + j], cc = t[8 + j], d = t[12 + j];
    f[j] = a + b + cc + d;
    f[4 + j] = a + b - cc - d;
    f[8 + j] = a - b - cc + d;
    f[12 + j] = a - b + cc - d;
  }
  const int qp = qp_;
  const int ls = level_scale4_[0][qp % 6][0];
  const int qpd = qp / 6;
  for (int i = 0; i < 16; ++i) {
    const int64_t v = static_cast<int64_t>(f[i]) * ls;
    dc_luma_[i] = static_cast<int32_t>(qpd >= 6 ? v << (qpd - 6) : (v + (1 << (5 - qpd))) >> (6 - qpd));
  }
}

void Core::finish_chroma_dc(int plane, const int* lvl) {
  const int qpc = cm_->qpc[plane - 1];
  const int list = (mb_type_ != kInter ? 0 : 3) + plane;
  const int c0 = lvl[0], c1 = lvl[1], c2 = lvl[2], c3 = lvl[3];
  const int f[4] = {c0 + c1 + c2 + c3, c0 - c1 + c2 - c3, c0 + c1 - c2 - c3, c0 - c1 - c2 + c3};
  const int ls = level_scale4_[list][qpc % 6][0];
  for (int i = 0; i < 4; ++i) {
    const int64_t v = (static_cast<int64_t>(f[i]) * ls << (qpc / 6)) >> 5;
    coef_[plane][i * 16] = static_cast<int32_t>(v);
    if (f[i]) coef_present_[plane][i] = true;
  }
}

bool Core::read_residual_cavlc(BitReader& br) {
  MbCtx* m = cm_;
  int lvl[64];
  const int qp = qp_;
  const bool intra = mb_type_ != kInter;
  const int list_y = intra ? 0 : 3;
  if (mb_type_ == kI16) {
    const int n = cavlc_residual(br, cavlc_nc(0, 0, 0), 16, lvl);
    if (n < 0) return false;
    finish_luma_dc(lvl);
    if (n > 0) m->cbf_dc |= 1;
  }
  for (int b8 = 0; b8 < 4; ++b8) {
    if (!((cbp_ >> b8) & 1)) continue;
    const int qx = (b8 & 1) * 2, qy = (b8 >> 1) * 2;
    if (m->t8x8) {
      int l64[64];
      for (int i = 0; i < 64; ++i) l64[i] = 0;
      int sum = 0;
      for (int s = 0; s < 4; ++s) {
        const int bx = qx + (s & 1), by = qy + (s >> 1);
        const int n = cavlc_residual(br, cavlc_nc(bx, by, 0), 16, lvl);
        if (n < 0) return false;
        m->nz[by * 4 + bx] = static_cast<uint8_t>(n);
        sum += n;
        for (int i = 0; i < 16; ++i) l64[i * 4 + s] = lvl[i];
      }
      if (sum) {
        store8x8(&coef_[0][b8 * 64], l64, intra ? 0 : 1, qp);
        coef_present_[0][b8] = true;
        for (int s = 0; s < 4; ++s) m->nzmask = static_cast<uint16_t>(m->nzmask | (1u << ((qy + (s >> 1)) * 4 + qx + (s & 1))));
      }
    } else {
      for (int s = 0; s < 4; ++s) {
        const int bx = qx + (s & 1), by = qy + (s >> 1);
        const int blk = by * 4 + bx;
        int n;
        if (mb_type_ == kI16) {
          n = cavlc_residual(br, cavlc_nc(bx, by, 0), 15, lvl);
          if (n < 0) return false;
          if (n) store4x4(&coef_[0][blk * 16], lvl, 1, 15, 0, qp);
        } else {
          n = cavlc_residual(br, cavlc_nc(bx, by, 0), 16, lvl);
          if (n < 0) return false;
          if (n) store4x4(&coef_[0][blk * 16], lvl, 0, 16, list_y, qp);
        }
        m->nz[blk] = static_cast<uint8_t>(n);
        if (n) {
          coef_present_[0][blk] = true;
          m->nzmask = static_cast<uint16_t>(m->nzmask | (1u << blk));
        }
      }
    }
  }
  if ((cbp_ >> 4) && !mono_) {
    int dc[2][4];
    for (int c = 0; c < 2; ++c) {
      const int n = cavlc_residual(br, -1, 4, lvl);
      if (n < 0) return false;
      for (int i = 0; i < 4; ++i) dc[c][i] = lvl[i];
      if (n) m->cbf_dc = static_cast<uint8_t>(m->cbf_dc | (2 << c));
    }
    for (int c = 0; c < 2; ++c) finish_chroma_dc(c + 1, dc[c]);
    if (cbp_ >> 4 == 2) {
      for (int c = 0; c < 2; ++c) {
        const int plane = c + 1;
        const int list = (intra ? 0 : 3) + plane;
        for (int s = 0; s < 4; ++s) {
          const int bx = s & 1, by = s >> 1;
          const int n = cavlc_residual(br, cavlc_nc(bx, by, plane), 15, lvl);
          if (n < 0) return false;
          m->nz[16 + c * 4 + s] = static_cast<uint8_t>(n);
          if (n) {
            store4x4(&coef_[plane][s * 16], lvl, 1, 15, list, m->qpc[c]);
            coef_present_[plane][s] = true;
          }
        }
      }
    }
  }
  return !br.error();
}

}  // namespace h264
}  // namespace hardware
