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

// The H.264 picture encoder (H264PictureEncoder.hpp). Everything it predicts from is what a decoder has at the same point (the reconstruction of the picture so far, the
// program's reference pictures), so the reconstruction it writes is bit for bit what a decoder makes of the slices. Clause numbers are ITU-T H.264's.

#include "Hardware/VideoCodec/Software/H264PictureEncoder.hpp"
#include "Hardware/VideoCodec/Software/H264Core.hpp"
#include "Hardware/VideoCodec/Software/MotionEstimator.hpp"

namespace hardware {
namespace h264 {

#include "Hardware/VideoCodec/Software/H264Tables.inc"

namespace {

inline int clip3(int lo, int hi, int v) { return v < lo ? lo : v > hi ? hi : v; }
inline int iabs(int v) { return v < 0 ? -v : v; }
inline int imin(int a, int b) { return a < b ? a : b; }
inline int imax(int a, int b) { return a > b ? a : b; }
inline uint8_t clip_pixel(int v) { return static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v); }
inline int median3(int a, int b, int c) { return imax(imin(a, b), imin(imax(a, b), c)); }

// Bits MSB first into an RBSP.
class BitWriter {
 public:
  explicit BitWriter(allocators::IAllocator& allocator) : buf_(allocator) {}
  void reset() {
    buf_.clear();
    acc_ = 0;
    count_ = 0;
    ok_ = true;
  }
  void bit(uint32_t b) {
    acc_ = static_cast<uint8_t>((acc_ << 1) | (b & 1));
    if (++count_ == 8) {
      ok_ = buf_.put8(acc_) && ok_;
      acc_ = 0;
      count_ = 0;
    }
  }
  void put(uint32_t value, int n) {
    for (int i = n - 1; i >= 0; --i) bit(value >> i);
  }
  void ue(uint32_t v) {
    const uint64_t x = uint64_t{v} + 1;
    int len = 0;
    while ((x >> (len + 1)) != 0) ++len;
    put(0, len);
    for (int i = len; i >= 0; --i) bit(static_cast<uint32_t>(x >> i));
  }
  void se(int v) { ue(v > 0 ? static_cast<uint32_t>(2 * v - 1) : static_cast<uint32_t>(-2 * v)); }
  void trailing() {
    bit(1);
    while (count_ != 0) bit(0);
  }
  uint64_t bits() const { return buf_.size() * 8 + static_cast<uint64_t>(count_); }
  void fail() { ok_ = false; }
  bool ok() const { return ok_; }
  ConstByteView bytes() const { return buf_.view(); }

 private:
  ByteBuffer buf_;
  uint8_t acc_ = 0;
  int count_ = 0;
  bool ok_ = true;
};

struct VlcRef {
  const VlcEntry* entries;
  int count;
};
template <int N>
constexpr VlcRef vlc(const VlcEntry (&a)[N]) {
  return {a, N};
}
const VlcRef kCoeffTokenTables[4] = {vlc(kCoeffToken0), vlc(kCoeffToken1), vlc(kCoeffToken2), vlc(kCoeffToken3)};
const VlcRef kCoeffTokenDc = vlc(kCoeffTokenChromaDc);
const VlcRef kTotalZerosTables[15] = {vlc(kTotalZeros1),  vlc(kTotalZeros2),  vlc(kTotalZeros3),  vlc(kTotalZeros4),  vlc(kTotalZeros5),
                                      vlc(kTotalZeros6),  vlc(kTotalZeros7),  vlc(kTotalZeros8),  vlc(kTotalZeros9),  vlc(kTotalZeros10),
                                      vlc(kTotalZeros11), vlc(kTotalZeros12), vlc(kTotalZeros13), vlc(kTotalZeros14), vlc(kTotalZeros15)};
const VlcRef kTotalZerosDcTables[3] = {vlc(kTotalZerosChromaDc1), vlc(kTotalZerosChromaDc2), vlc(kTotalZerosChromaDc3)};
const VlcRef kRunBeforeTables[7] = {vlc(kRunBefore1), vlc(kRunBefore2), vlc(kRunBefore3), vlc(kRunBefore4), vlc(kRunBefore5), vlc(kRunBefore6), vlc(kRunBefore7)};

void put_vlc(BitWriter& w, const VlcRef& table, int symbol) {
  for (int i = 0; i < table.count; ++i) {
    if (table.entries[i].sym == symbol) {
      w.put(table.entries[i].code, table.entries[i].len);
      return;
    }
  }
  w.fail();
}

// 9.2 in reverse: one residual block of max_coeff levels in scan order; nc < 0 is the chroma DC block. Returns TotalCoeff.
int write_block(BitWriter& w, int nc, const int* lvl, int max_coeff) {
  int vals[16], pos[16];
  int total = 0;
  for (int i = max_coeff - 1; i >= 0; --i) {
    if (lvl[i] != 0) {
      vals[total] = lvl[i];
      pos[total] = i;
      ++total;
    }
  }
  int t1 = 0;
  while (t1 < total && t1 < 3 && iabs(vals[t1]) == 1) ++t1;
  put_vlc(w, nc < 0 ? kCoeffTokenDc : kCoeffTokenTables[nc < 2 ? 0 : nc < 4 ? 1 : nc < 8 ? 2 : 3], total * 4 + t1);
  if (total == 0) return 0;
  for (int i = 0; i < t1; ++i) w.bit(vals[i] < 0);
  int suffix_len = (total > 10 && t1 < 3) ? 1 : 0;
  for (int i = t1; i < total; ++i) {
    const int v = vals[i];
    int code = v > 0 ? 2 * v - 2 : -2 * v - 1;
    if (i == t1 && t1 < 3) code -= 2;
    int prefix, suffix = 0, suffix_bits = 0;
    if (suffix_len == 0) {
      if (code < 14) {
        prefix = code;
      } else if (code < 30) {
        prefix = 14;
        suffix = code - 14;
        suffix_bits = 4;
      } else {
        prefix = 15;
        suffix = code - 30;
        suffix_bits = 12;
      }
    } else if (code < (15 << suffix_len)) {
      prefix = code >> suffix_len;
      suffix = code & ((1 << suffix_len) - 1);
      suffix_bits = suffix_len;
    } else {
      prefix = 15;
      suffix = code - (15 << suffix_len);
      suffix_bits = 12;
    }
    if (suffix_bits > 0 && suffix >= (1 << suffix_bits)) w.fail();   // (levels are clamped so that this cannot happen)
    w.put(1, prefix + 1);
    if (suffix_bits > 0) w.put(static_cast<uint32_t>(suffix), suffix_bits);
    if (suffix_len == 0) suffix_len = 1;
    if (iabs(v) > (3 << (suffix_len - 1)) && suffix_len < 6) ++suffix_len;
  }
  int zeros_left = 0;
  if (total < max_coeff) {
    zeros_left = pos[0] + 1 - total;
    put_vlc(w, nc < 0 ? kTotalZerosDcTables[total - 1] : kTotalZerosTables[total - 1], zeros_left);
  }
  for (int i = 0; i < total - 1 && zeros_left > 0; ++i) {
    const int run = pos[i] - pos[i + 1] - 1;
    put_vlc(w, kRunBeforeTables[imin(zeros_left, 7) - 1], run);
    zeros_left -= run;
  }
  return total;
}

const int kMf[6][3] = {{13107, 5243, 8066}, {11916, 4660, 7490}, {10082, 4194, 6554}, {9362, 3647, 5825}, {8192, 3355, 5243}, {7282, 2893, 4559}};
const int kNorm[6][3] = {{10, 16, 13}, {11, 18, 14}, {13, 20, 16}, {14, 23, 18}, {16, 25, 20}, {18, 29, 23}};
const int kMaxLevel = 2047;   // keeps level_prefix within 15 (no escape beyond what every profile allows)
// x264's lambda for SAD-based decisions, by QP.
const uint8_t kLambda[52] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 6, 6, 7, 8, 9, 10, 11, 13, 14, 16, 18, 20, 23, 25, 29, 32};

inline int pos_class(int p) {
  const int i = p >> 2, j = p & 3;
  return ((i & 1) == 0 && (j & 1) == 0) ? 0 : ((i & 1) && (j & 1)) ? 1 : 2;
}
inline int quantize(int w, int mf, int shift, int round) {
  const int64_t q = (static_cast<int64_t>(iabs(w)) * mf + round) >> shift;
  const int level = static_cast<int>(q > kMaxLevel ? kMaxLevel : q);
  return w < 0 ? -level : level;
}
void forward4x4(const int* r, int* w) {
  int t[16];
  for (int i = 0; i < 4; ++i) {
    const int a = r[i * 4], b = r[i * 4 + 1], c = r[i * 4 + 2], d = r[i * 4 + 3];
    const int s0 = a + d, s1 = b + c, d0 = a - d, d1 = b - c;
    t[i * 4 + 0] = s0 + s1;
    t[i * 4 + 1] = 2 * d0 + d1;
    t[i * 4 + 2] = s0 - s1;
    t[i * 4 + 3] = d0 - 2 * d1;
  }
  for (int j = 0; j < 4; ++j) {
    const int a = t[j], b = t[4 + j], c = t[8 + j], d = t[12 + j];
    const int s0 = a + d, s1 = b + c, d0 = a - d, d1 = b - c;
    w[j] = s0 + s1;
    w[4 + j] = 2 * d0 + d1;
    w[8 + j] = s0 - s1;
    w[12 + j] = d0 - 2 * d1;
  }
}
// 8.5.12: dequantised coefficients (raster) transformed and added to the samples at `offset` of `dst`.
void inverse4x4_add(const int* d, ByteView dst, uint64_t offset, uint32_t stride) {
  int t[16];
  for (int i = 0; i < 4; ++i) {
    const int e0 = d[i * 4] + d[i * 4 + 2], e1 = d[i * 4] - d[i * 4 + 2];
    const int e2 = (d[i * 4 + 1] >> 1) - d[i * 4 + 3], e3 = d[i * 4 + 1] + (d[i * 4 + 3] >> 1);
    t[i * 4 + 0] = e0 + e3;
    t[i * 4 + 1] = e1 + e2;
    t[i * 4 + 2] = e1 - e2;
    t[i * 4 + 3] = e0 - e3;
  }
  for (int j = 0; j < 4; ++j) {
    const int e0 = t[j] + t[8 + j], e1 = t[j] - t[8 + j];
    const int e2 = (t[4 + j] >> 1) - t[12 + j], e3 = t[4 + j] + (t[12 + j] >> 1);
    const int r[4] = {e0 + e3, e1 + e2, e1 - e2, e0 - e3};
    for (int i = 0; i < 4; ++i) {
      const uint64_t at = offset + uint64_t{static_cast<uint32_t>(i)} * stride + static_cast<uint32_t>(j);
      dst[at] = clip_pixel(dst[at] + ((r[i] + 32) >> 6));
    }
  }
}
inline int dequant(int level, int qp, int pos) { return (level * kNorm[qp % 6][pos_class(pos)]) << (qp / 6); }

// The bits of se(v) of v.
inline int se_bits(int v) {
  const uint32_t code = v > 0 ? static_cast<uint32_t>(2 * v - 1) : static_cast<uint32_t>(-2 * v);
  int len = 0;
  while (((uint64_t{code} + 1) >> (len + 1)) != 0) ++len;
  return 2 * len + 1;
}

// 8.7.2.3 / 8.7.2.4: filters `count` sample lines across one edge of `plane`; q0 of line i is at q + i * along, q1 at q0 + across.
void filter_edge(ByteView plane, int64_t q, int64_t across, int64_t along, int count, const int* bs, int index_a, int alpha, int beta, bool chroma, int lines_per_bs) {
  auto px = [&](int64_t at) -> uint8_t& { return plane[static_cast<uint64_t>(at)]; };
  for (int i = 0; i < count; ++i) {
    const int s = bs[i / lines_per_bs];
    if (s == 0) continue;
    const int64_t at = q + i * along;
    const int p0 = px(at - across), p1 = px(at - 2 * across), q0 = px(at), q1 = px(at + across);
    if (!(iabs(p0 - q0) < alpha && iabs(p1 - p0) < beta && iabs(q1 - q0) < beta)) continue;
    if (s < 4) {
      const int tc0 = kTc0[index_a * 4 + s];
      if (chroma) {
        const int tc = tc0 + 1;
        const int delta = clip3(-tc, tc, (((q0 - p0) << 2) + (p1 - q1) + 4) >> 3);
        px(at - across) = clip_pixel(p0 + delta);
        px(at) = clip_pixel(q0 - delta);
      } else {
        const int p2 = px(at - 3 * across), q2 = px(at + 2 * across);
        const int ap = iabs(p2 - p0), aq = iabs(q2 - q0);
        const int tc = tc0 + (ap < beta) + (aq < beta);
        const int delta = clip3(-tc, tc, (((q0 - p0) << 2) + (p1 - q1) + 4) >> 3);
        px(at - across) = clip_pixel(p0 + delta);
        px(at) = clip_pixel(q0 - delta);
        if (ap < beta) px(at - 2 * across) = static_cast<uint8_t>(p1 + clip3(-tc0, tc0, (p2 + ((p0 + q0 + 1) >> 1) - (p1 << 1)) >> 1));
        if (aq < beta) px(at + across) = static_cast<uint8_t>(q1 + clip3(-tc0, tc0, (q2 + ((p0 + q0 + 1) >> 1) - (q1 << 1)) >> 1));
      }
    } else if (chroma) {
      px(at - across) = static_cast<uint8_t>((2 * p1 + p0 + q1 + 2) >> 2);
      px(at) = static_cast<uint8_t>((2 * q1 + q0 + p1 + 2) >> 2);
    } else {
      const int p2 = px(at - 3 * across), q2 = px(at + 2 * across);
      const int ap = iabs(p2 - p0), aq = iabs(q2 - q0);
      const bool small = iabs(p0 - q0) < ((alpha >> 2) + 2);
      if (ap < beta && small) {
        const int p3 = px(at - 4 * across);
        px(at - across) = static_cast<uint8_t>((p2 + 2 * p1 + 2 * p0 + 2 * q0 + q1 + 4) >> 3);
        px(at - 2 * across) = static_cast<uint8_t>((p2 + p1 + p0 + q0 + 2) >> 2);
        px(at - 3 * across) = static_cast<uint8_t>((2 * p3 + 3 * p2 + p1 + p0 + q0 + 4) >> 3);
      } else {
        px(at - across) = static_cast<uint8_t>((2 * p1 + p0 + q1 + 2) >> 2);
      }
      if (aq < beta && small) {
        const int q3 = px(at + 3 * across);
        px(at) = static_cast<uint8_t>((p1 + 2 * p0 + 2 * q0 + 2 * q1 + q2 + 4) >> 3);
        px(at + across) = static_cast<uint8_t>((p0 + q0 + q1 + q2 + 2) >> 2);
        px(at + 2 * across) = static_cast<uint8_t>((2 * q3 + 3 * q2 + q1 + q0 + p0 + 4) >> 3);
      } else {
        px(at) = static_cast<uint8_t>((2 * q1 + q0 + p1 + 2) >> 2);
      }
    }
  }
}

// What the coded picture keeps of a macroblock (for the neighbours' predictions, CAVLC contexts and the in-loop filter).
struct MbData {
  int32_t slice = -1;
  bool intra = false;
  uint8_t qp = 0;
  int8_t ref[2] = {-1, -1};          // reference index per list (-1: the list is not used)
  int64_t ident[2] = {-1, -1};       // the identity of that reference picture
  int16_t mv[2][2] = {};
  uint8_t nz[24] = {};               // TotalCoeff of each 4x4 block: 16 luma (raster), 4 Cb, 4 Cr
  uint16_t nzmask = 0;               // luma 4x4 blocks with coefficients
};

struct Vec {
  int x = 0, y = 0;
};
inline bool operator==(Vec a, Vec b) { return a.x == b.x && a.y == b.y; }

}  // namespace

class PictureCoder {
 public:
  explicit PictureCoder(allocators::IAllocator& allocator) : bw_(allocator), nal_(allocator), mbs_(allocator), starts_(allocator) {}

  H264EncResult encode(const H264EncPicture& pic, const H264EncPlanes& src, const H264EncWritablePlanes& rec, ByteBuffer* out, List<H264EncSlice>* slices, H264EncStatistics* stats);

 private:
  // The macroblock being coded.
  struct Mb {
    bool intra = true;
    int i16_mode = 2, chroma_mode = 0;
    int pred_flags = 1;                // inter: bit 0 list 0, bit 1 list 1
    int ref[2] = {0, 0};
    Vec mv[2];
    int qp = 26, qpc = 26;
    int cbp_luma = 0, cbp_chroma = 0;
    int dc[16];
    int ac[16][16];
    int cdc[2][4];
    int cac[2][4][16];
    uint8_t pred_y[256];
    uint8_t pred_c[2][64];
    uint8_t nz[24];
  };
  struct Neighbour {
    bool available = false;
    int ref = -1;
    Vec mv;
  };

  bool p_slice() const { return pic_->type == H264EncFrameType::P; }
  bool b_slice() const { return pic_->type == H264EncFrameType::B; }
  uint64_t addr(int x, int y) const { return static_cast<uint64_t>(y) * mbw_ + static_cast<uint64_t>(x); }
  bool available(int x, int y) const { return x >= 0 && y >= 0 && x < static_cast<int>(mbw_) && y < static_cast<int>(mbh_) && mbs_[addr(x, y)].slice == slice_; }
  // (for intra prediction: constrained intra prediction leaves out inter macroblocks)
  bool intra_available(int x, int y) const { return available(x, y) && (!pic_->constrained_intra_pred || mbs_[addr(x, y)].intra); }

  Neighbour neighbour(int x, int y, int list) const;
  Vec predict_mv(int mbx, int mby, int list, int ref) const;
  Vec skip_mv(int mbx, int mby) const;
  int nc_for(int bx, int by, int plane, int mbx, int mby, const uint8_t* cur) const;

  bool pred_i16(int mode, int mbx, int mby, uint8_t* pred) const;
  bool pred_chroma(int mode, int mbx, int mby, int plane, uint8_t* pred) const;
  void pred_luma(int list, int ref, Vec mv, int mbx, int mby, uint8_t* out) const;
  void pred_chroma_inter(int list, int ref, Vec mv, int mbx, int mby, uint8_t out[2][64]) const;
  uint32_t sad16(int mbx, int mby, const uint8_t* pred) const;
  int mv_cost(Vec mv, Vec pred) const { return lambda_ * (se_bits(mv.x - pred.x) + se_bits(mv.y - pred.y)); }
  int ref_cost(int list, int ref) const;
  // The best vector into list[list][ref] (diamond search, then half and quarter refinement as the precision allows): cost (SAD + rate) and vector.
  int search(int mbx, int mby, int list, int ref, Vec pred, Vec* best) const;

  void quant_luma_i16(int mbx, int mby);
  void quant_luma_inter(int mbx, int mby);
  void quant_chroma(int mbx, int mby, bool intra);
  void reconstruct(int mbx, int mby);
  void write_mb(int mbx, int mby, int qp_delta, const Vec* mvp);
  void write_slice_header(uint32_t first_mb);
  void code_mb(int mbx, int mby);

  int boundary_strength(const MbData& p, const MbData& q, int blk_p, int blk_q, bool mb_edge) const;
  void deblock_mb(int mbx, int mby);

  const H264EncPicture* pic_ = nullptr;
  H264EncPlanes src_;
  H264EncWritablePlanes rec_;
  BitWriter bw_;
  ByteBuffer nal_;
  List<MbData> mbs_;
  List<uint32_t> starts_;
  uint32_t mbw_ = 0, mbh_ = 0;
  int32_t slice_ = 0;
  int qp_prev_ = 26;
  int lambda_ = 1;
  uint32_t skip_run_ = 0;
  Mb m_;
  H264EncStatistics stats_;
};

// ---------------------------------------------------------------------------------------------------------------
// Neighbours

PictureCoder::Neighbour PictureCoder::neighbour(int x, int y, int list) const {
  Neighbour n;
  if (!available(x, y)) return n;
  n.available = true;
  const MbData& m = mbs_[addr(x, y)];
  if (m.intra || m.ref[list] < 0) return n;
  n.ref = m.ref[list];
  n.mv = {m.mv[list][0], m.mv[list][1]};
  return n;
}

// 8.4.1.3: the predicted vector of a 16x16 partition with reference index `ref` of list `list`.
Vec PictureCoder::predict_mv(int mbx, int mby, int list, int ref) const {
  Neighbour a = neighbour(mbx - 1, mby, list), b = neighbour(mbx, mby - 1, list), c = neighbour(mbx + 1, mby - 1, list);
  if (!c.available) c = neighbour(mbx - 1, mby - 1, list);
  if (!b.available && !c.available && a.available) {
    b = a;
    c = a;
  }
  const int same = (a.ref == ref) + (b.ref == ref) + (c.ref == ref);
  if (same == 1) return a.ref == ref ? a.mv : b.ref == ref ? b.mv : c.mv;
  return {median3(a.mv.x, b.mv.x, c.mv.x), median3(a.mv.y, b.mv.y, c.mv.y)};
}

// 8.4.1.1: the vector of a P_Skip macroblock.
Vec PictureCoder::skip_mv(int mbx, int mby) const {
  const Neighbour a = neighbour(mbx - 1, mby, 0), b = neighbour(mbx, mby - 1, 0);
  if (!a.available || !b.available || (a.ref == 0 && a.mv == Vec{}) || (b.ref == 0 && b.mv == Vec{})) return {};
  return predict_mv(mbx, mby, 0, 0);
}

// 9.2.1: nC of a 4x4 block; plane 0 luma (bx, by 0 .. 3), 1 and 2 chroma (0 .. 1).
int PictureCoder::nc_for(int bx, int by, int plane, int mbx, int mby, const uint8_t* cur) const {
  const int w = plane == 0 ? 4 : 2;
  const int base = plane == 0 ? 0 : 16 + (plane - 1) * 4;
  int na = -1, nb = -1;
  if (bx > 0) na = cur[base + by * w + bx - 1];
  else if (available(mbx - 1, mby)) na = mbs_[addr(mbx - 1, mby)].nz[base + by * w + w - 1];
  if (by > 0) nb = cur[base + (by - 1) * w + bx];
  else if (available(mbx, mby - 1)) nb = mbs_[addr(mbx, mby - 1)].nz[base + (w - 1) * w + bx];
  if (na >= 0 && nb >= 0) return (na + nb + 1) >> 1;
  if (na >= 0) return na;
  if (nb >= 0) return nb;
  return 0;
}

// ---------------------------------------------------------------------------------------------------------------
// Prediction

// 8.3.3: Intra16x16 prediction (0 vertical, 1 horizontal, 2 DC, 3 plane). False if the mode needs samples that are not available.
bool PictureCoder::pred_i16(int mode, int mbx, int mby, uint8_t* pred) const {
  const uint32_t W = rec_.width;
  const uint64_t base = uint64_t{static_cast<uint32_t>(mby)} * 16 * W + static_cast<uint32_t>(mbx) * 16;
  const bool left = intra_available(mbx - 1, mby), top = intra_available(mbx, mby - 1), corner = intra_available(mbx - 1, mby - 1);
  auto T = [&](int x) -> int { return rec_.y[base - W + static_cast<uint64_t>(x)]; };                              // p[x, -1]
  auto L = [&](int y) -> int { return rec_.y[base + static_cast<uint64_t>(y) * W - 1]; };                          // p[-1, y]
  if (mode == 0) {
    if (!top) return false;
    for (int y = 0; y < 16; ++y)
      for (int x = 0; x < 16; ++x) pred[y * 16 + x] = static_cast<uint8_t>(T(x));
    return true;
  }
  if (mode == 1) {
    if (!left) return false;
    for (int y = 0; y < 16; ++y)
      for (int x = 0; x < 16; ++x) pred[y * 16 + x] = static_cast<uint8_t>(L(y));
    return true;
  }
  if (mode == 2) {
    int sum = 0, dc;
    if (top) for (int x = 0; x < 16; ++x) sum += T(x);
    if (left) for (int y = 0; y < 16; ++y) sum += L(y);
    if (top && left) dc = (sum + 16) >> 5;
    else if (top || left) dc = (sum + 8) >> 4;
    else dc = 128;
    for (int i = 0; i < 256; ++i) pred[i] = static_cast<uint8_t>(dc);
    return true;
  }
  if (mode != 3 || !top || !left || !corner) return false;
  const int corner_sample = rec_.y[base - W - 1];
  int H = 0, V = 0;
  for (int i = 0; i < 8; ++i) {
    H += (i + 1) * (T(8 + i) - (6 - i >= 0 ? T(6 - i) : corner_sample));
    V += (i + 1) * (L(8 + i) - (6 - i >= 0 ? L(6 - i) : corner_sample));
  }
  const int a = 16 * (L(15) + T(15)), b = (5 * H + 32) >> 6, c = (5 * V + 32) >> 6;
  for (int y = 0; y < 16; ++y)
    for (int x = 0; x < 16; ++x) pred[y * 16 + x] = clip_pixel((a + b * (x - 7) + c * (y - 7) + 16) >> 5);
  return true;
}

// 8.3.4: chroma prediction of one plane (0 DC, 1 horizontal, 2 vertical, 3 plane).
bool PictureCoder::pred_chroma(int mode, int mbx, int mby, int plane, uint8_t* pred) const {
  const uint32_t W = rec_.width / 2;
  const ByteView& p = plane == 0 ? rec_.u : rec_.v;
  const uint64_t base = uint64_t{static_cast<uint32_t>(mby)} * 8 * W + static_cast<uint32_t>(mbx) * 8;
  const bool left = intra_available(mbx - 1, mby), top = intra_available(mbx, mby - 1), corner = intra_available(mbx - 1, mby - 1);
  auto T = [&](int x) -> int { return p[base - W + static_cast<uint64_t>(x)]; };
  auto L = [&](int y) -> int { return p[base + static_cast<uint64_t>(y) * W - 1]; };
  if (mode == 2) {
    if (!top) return false;
    for (int y = 0; y < 8; ++y)
      for (int x = 0; x < 8; ++x) pred[y * 8 + x] = static_cast<uint8_t>(T(x));
    return true;
  }
  if (mode == 1) {
    if (!left) return false;
    for (int y = 0; y < 8; ++y)
      for (int x = 0; x < 8; ++x) pred[y * 8 + x] = static_cast<uint8_t>(L(y));
    return true;
  }
  if (mode == 3) {
    if (!top || !left || !corner) return false;
    const int corner_sample = p[base - W - 1];
    int H = 0, V = 0;
    for (int i = 0; i < 4; ++i) {
      H += (i + 1) * (T(4 + i) - (2 - i >= 0 ? T(2 - i) : corner_sample));
      V += (i + 1) * (L(4 + i) - (2 - i >= 0 ? L(2 - i) : corner_sample));
    }
    const int a = 16 * (L(7) + T(7)), b = (34 * H + 32) >> 6, c = (34 * V + 32) >> 6;
    for (int y = 0; y < 8; ++y)
      for (int x = 0; x < 8; ++x) pred[y * 8 + x] = clip_pixel((a + b * (x - 3) + c * (y - 3) + 16) >> 5);
    return true;
  }
  if (mode != 0) return false;
  for (int by = 0; by < 2; ++by) {
    for (int bx = 0; bx < 2; ++bx) {
      int st = 0, sl = 0;
      if (top) for (int x = 0; x < 4; ++x) st += T(bx * 4 + x);
      if (left) for (int y = 0; y < 4; ++y) sl += L(by * 4 + y);
      int dc;
      if ((bx == 0 && by == 0) || (bx == 1 && by == 1)) {
        if (top && left) dc = (st + sl + 4) >> 3;
        else if (left) dc = (sl + 2) >> 2;
        else if (top) dc = (st + 2) >> 2;
        else dc = 128;
      } else if (bx == 1) {
        if (top) dc = (st + 2) >> 2;
        else if (left) dc = (sl + 2) >> 2;
        else dc = 128;
      } else {
        if (left) dc = (sl + 2) >> 2;
        else if (top) dc = (st + 2) >> 2;
        else dc = 128;
      }
      for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) pred[(by * 4 + y) * 8 + bx * 4 + x] = static_cast<uint8_t>(dc);
    }
  }
  return true;
}

void PictureCoder::pred_luma(int list, int ref, Vec mv, int mbx, int mby, uint8_t* out) const {
  const H264EncPlanes& r = pic_->list[list][static_cast<uint64_t>(ref)].picture;
  const LumaPlane plane{r.y, static_cast<int>(r.width), static_cast<int>(r.height), static_cast<int>(r.width)};
  h264_luma_prediction(plane, mbx * 16 + (mv.x >> 2), mby * 16 + (mv.y >> 2), mv.x & 3, mv.y & 3, 16, 16, ByteView(out, 256));
}

// 8.4.2.2.2: chroma sample interpolation (eighth samples, bilinear), edge samples repeated.
void PictureCoder::pred_chroma_inter(int list, int ref, Vec mv, int mbx, int mby, uint8_t out[2][64]) const {
  const H264EncPlanes& r = pic_->list[list][static_cast<uint64_t>(ref)].picture;
  const int CW = static_cast<int>(r.width / 2), CH = static_cast<int>(r.height / 2);
  const int cx0 = mbx * 8 + (mv.x >> 3), cy0 = mby * 8 + (mv.y >> 3), fx = mv.x & 7, fy = mv.y & 7;
  for (int c = 0; c < 2; ++c) {
    const ConstByteView& p = c == 0 ? r.u : r.v;
    for (int y = 0; y < 8; ++y) {
      const int ya = clip3(0, CH - 1, cy0 + y), yb = clip3(0, CH - 1, cy0 + y + 1);
      for (int x = 0; x < 8; ++x) {
        const int xa = clip3(0, CW - 1, cx0 + x), xb = clip3(0, CW - 1, cx0 + x + 1);
        const int A = p[static_cast<uint64_t>(ya) * CW + xa], B = p[static_cast<uint64_t>(ya) * CW + xb];
        const int C = p[static_cast<uint64_t>(yb) * CW + xa], D = p[static_cast<uint64_t>(yb) * CW + xb];
        out[c][y * 8 + x] = static_cast<uint8_t>(((8 - fx) * (8 - fy) * A + fx * (8 - fy) * B + (8 - fx) * fy * C + fx * fy * D + 32) >> 6);
      }
    }
  }
}

uint32_t PictureCoder::sad16(int mbx, int mby, const uint8_t* pred) const {
  const uint32_t W = src_.width;
  const uint64_t base = uint64_t{static_cast<uint32_t>(mby)} * 16 * W + static_cast<uint32_t>(mbx) * 16;
  uint32_t sad = 0;
  for (int y = 0; y < 16; ++y) {
    const ConstByteView row = src_.y.subview(base + uint64_t{static_cast<uint32_t>(y)} * W, 16);
    for (int x = 0; x < 16; ++x) sad += static_cast<uint32_t>(iabs(int{row[static_cast<uint64_t>(x)]} - pred[y * 16 + x]));
  }
  return sad;
}

int PictureCoder::ref_cost(int list, int ref) const {
  const uint64_t n = pic_->list[list].size();
  if (n <= 1) return 0;
  if (n == 2) return lambda_;
  int len = 0;
  while (((static_cast<uint64_t>(ref) + 1) >> (len + 1)) != 0) ++len;
  return lambda_ * (2 * len + 1);
}

int PictureCoder::search(int mbx, int mby, int list, int ref, Vec pred, Vec* best) const {
  uint8_t buf[256];
  auto cost_of = [&](Vec v) {
    pred_luma(list, ref, v, mbx, mby, buf);
    return static_cast<int>(sad16(mbx, mby, buf)) + mv_cost(v, pred);
  };
  // (vectors stay within what every level allows: vertical [-256, 255.75] samples is level 1.1's... and the search keeps to +-64 vertically, +-256 horizontally)
  auto allowed = [](Vec v) { return v.x >= -1024 && v.x <= 1020 && v.y >= -256 && v.y <= 252; };
  Vec b{};
  int bc = 0x7FFFFFFF;
  auto consider = [&](Vec v) {
    if (!allowed(v)) return;
    const int c = cost_of(v);
    if (c < bc) {
      bc = c;
      b = v;
    }
  };
  auto whole = [](Vec v) { return Vec{v.x & ~3, v.y & ~3}; };
  consider({0, 0});
  consider(whole(pred));
  for (int dir = 0; dir < 3; ++dir) {
    const int x = dir == 0 ? mbx - 1 : dir == 1 ? mbx : mbx + 1, y = dir == 0 ? mby : mby - 1;
    const Neighbour n = neighbour(x, y, list);
    if (n.ref >= 0) consider(whole(n.mv));
  }
  for (int iter = 0; iter < 32; ++iter) {
    const Vec c = b;
    static const int kLarge[8][2] = {{0, -8}, {0, 8}, {-8, 0}, {8, 0}, {-4, -4}, {4, -4}, {-4, 4}, {4, 4}};
    for (const auto& d : kLarge) consider({c.x + d[0], c.y + d[1]});
    if (b == c) break;
  }
  for (int iter = 0; iter < 8; ++iter) {
    const Vec c = b;
    static const int kSmall[4][2] = {{0, -4}, {0, 4}, {-4, 0}, {4, 0}};
    for (const auto& d : kSmall) consider({c.x + d[0], c.y + d[1]});
    if (b == c) break;
  }
  static const int kAround[8][2] = {{-1, -1}, {0, -1}, {1, -1}, {-1, 0}, {1, 0}, {-1, 1}, {0, 1}, {1, 1}};
  if (pic_->precision != H264EncPrecision::Full) {
    const Vec c = b;
    for (const auto& d : kAround) consider({c.x + 2 * d[0], c.y + 2 * d[1]});
  }
  if (pic_->precision == H264EncPrecision::Quarter) {
    const Vec c = b;
    for (const auto& d : kAround) consider({c.x + d[0], c.y + d[1]});
  }
  *best = b;
  return bc + ref_cost(list, ref);
}

// ---------------------------------------------------------------------------------------------------------------
// Residual

void PictureCoder::quant_luma_i16(int mbx, int mby) {
  const uint32_t W = src_.width;
  const uint64_t base = uint64_t{static_cast<uint32_t>(mby)} * 16 * W + static_cast<uint32_t>(mbx) * 16;
  const int qp = m_.qp, qpd = qp / 6, qpm = qp % 6;
  const int shift = 15 + qpd, round = (1 << shift) / 3;
  int dcm[16];
  bool any_ac = false;
  for (int blk = 0; blk < 16; ++blk) {
    const int bx = blk & 3, by = blk >> 2;
    int r[16], w[16];
    for (int y = 0; y < 4; ++y)
      for (int x = 0; x < 4; ++x) r[y * 4 + x] = int{src_.y[base + uint64_t{static_cast<uint32_t>(by * 4 + y)} * W + static_cast<uint32_t>(bx * 4 + x)]} - m_.pred_y[(by * 4 + y) * 16 + bx * 4 + x];
    forward4x4(r, w);
    dcm[blk] = w[0];
    m_.ac[blk][0] = 0;
    for (int i = 1; i < 16; ++i) {
      const int p = kZigzag4x4[i];
      m_.ac[blk][i] = quantize(w[p], kMf[qpm][pos_class(p)], shift, round);
      any_ac = any_ac || m_.ac[blk][i] != 0;
    }
  }
  int t[16], h[16];
  for (int i = 0; i < 4; ++i) {
    const int a = dcm[i * 4], b = dcm[i * 4 + 1], c = dcm[i * 4 + 2], d = dcm[i * 4 + 3];
    t[i * 4 + 0] = a + b + c + d;
    t[i * 4 + 1] = a + b - c - d;
    t[i * 4 + 2] = a - b - c + d;
    t[i * 4 + 3] = a - b + c - d;
  }
  for (int j = 0; j < 4; ++j) {
    const int a = t[j], b = t[4 + j], c = t[8 + j], d = t[12 + j];
    h[j] = a + b + c + d;
    h[4 + j] = a + b - c - d;
    h[8 + j] = a - b - c + d;
    h[12 + j] = a - b + c - d;
  }
  for (int i = 0; i < 16; ++i) m_.dc[i] = quantize(h[kZigzag4x4[i]], kMf[qpm][0], shift + 2, 4 * round);
  m_.cbp_luma = any_ac ? 15 : 0;
  if (!any_ac)
    for (int blk = 0; blk < 16; ++blk)
      for (int i = 1; i < 16; ++i) m_.ac[blk][i] = 0;
}

void PictureCoder::quant_luma_inter(int mbx, int mby) {
  const uint32_t W = src_.width;
  const uint64_t base = uint64_t{static_cast<uint32_t>(mby)} * 16 * W + static_cast<uint32_t>(mbx) * 16;
  const int qp = m_.qp, qpd = qp / 6, qpm = qp % 6;
  const int shift = 15 + qpd, round = (1 << shift) / 6;
  int cbp = 0;
  for (int blk = 0; blk < 16; ++blk) {
    const int bx = blk & 3, by = blk >> 2;
    int r[16], w[16];
    for (int y = 0; y < 4; ++y)
      for (int x = 0; x < 4; ++x) r[y * 4 + x] = int{src_.y[base + uint64_t{static_cast<uint32_t>(by * 4 + y)} * W + static_cast<uint32_t>(bx * 4 + x)]} - m_.pred_y[(by * 4 + y) * 16 + bx * 4 + x];
    forward4x4(r, w);
    bool nz = false;
    for (int i = 0; i < 16; ++i) {
      const int p = kZigzag4x4[i];
      m_.ac[blk][i] = quantize(w[p], kMf[qpm][pos_class(p)], shift, round);
      nz = nz || m_.ac[blk][i] != 0;
    }
    if (nz) cbp |= 1 << ((by >> 1) * 2 + (bx >> 1));
  }
  m_.cbp_luma = cbp;
}

void PictureCoder::quant_chroma(int mbx, int mby, bool intra) {
  const uint32_t W = src_.width / 2;
  const int qpd = m_.qpc / 6, qpm = m_.qpc % 6;
  const int shift = 15 + qpd, round = (1 << shift) / (intra ? 3 : 6);
  bool any_dc = false, any_ac = false;
  for (int c = 0; c < 2; ++c) {
    const ConstByteView& s = c == 0 ? src_.u : src_.v;
    const uint64_t base = uint64_t{static_cast<uint32_t>(mby)} * 8 * W + static_cast<uint32_t>(mbx) * 8;
    int dcv[4];
    for (int blk = 0; blk < 4; ++blk) {
      const int bx = blk & 1, by = blk >> 1;
      int r[16], w[16];
      for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) r[y * 4 + x] = int{s[base + uint64_t{static_cast<uint32_t>(by * 4 + y)} * W + static_cast<uint32_t>(bx * 4 + x)]} - m_.pred_c[c][(by * 4 + y) * 8 + bx * 4 + x];
      forward4x4(r, w);
      dcv[blk] = w[0];
      m_.cac[c][blk][0] = 0;
      for (int i = 1; i < 16; ++i) {
        const int p = kZigzag4x4[i];
        m_.cac[c][blk][i] = quantize(w[p], kMf[qpm][pos_class(p)], shift, round);
        any_ac = any_ac || m_.cac[c][blk][i] != 0;
      }
    }
    const int h[4] = {dcv[0] + dcv[1] + dcv[2] + dcv[3], dcv[0] - dcv[1] + dcv[2] - dcv[3], dcv[0] + dcv[1] - dcv[2] - dcv[3], dcv[0] - dcv[1] - dcv[2] + dcv[3]};
    for (int i = 0; i < 4; ++i) {
      m_.cdc[c][i] = quantize(h[i], kMf[qpm][0], shift + 1, 2 * round);
      any_dc = any_dc || m_.cdc[c][i] != 0;
    }
  }
  m_.cbp_chroma = any_ac ? 2 : any_dc ? 1 : 0;
  if (m_.cbp_chroma < 2)
    for (int c = 0; c < 2; ++c)
      for (int b = 0; b < 4; ++b)
        for (int i = 1; i < 16; ++i) m_.cac[c][b][i] = 0;
  if (m_.cbp_chroma == 0)
    for (int c = 0; c < 2; ++c)
      for (int i = 0; i < 4; ++i) m_.cdc[c][i] = 0;
}

// 8.5: prediction plus the inverse-transformed levels, as the decoder computes it.
void PictureCoder::reconstruct(int mbx, int mby) {
  const uint32_t W = rec_.width;
  const uint64_t base = uint64_t{static_cast<uint32_t>(mby)} * 16 * W + static_cast<uint32_t>(mbx) * 16;
  for (int y = 0; y < 16; ++y)
    for (int x = 0; x < 16; ++x) rec_.y[base + uint64_t{static_cast<uint32_t>(y)} * W + static_cast<uint32_t>(x)] = m_.pred_y[y * 16 + x];
  const int qp = m_.qp, qpc = m_.qpc;
  int dcl[16] = {};
  const bool i16 = m_.intra;
  if (i16) {   // 8.5.10: the DC levels, inverse Hadamard, scaled
    int c[16], t[16], f[16];
    for (int i = 0; i < 16; ++i) c[kZigzag4x4[i]] = m_.dc[i];
    for (int i = 0; i < 4; ++i) {
      const int a = c[i * 4], b = c[i * 4 + 1], cc = c[i * 4 + 2], d = c[i * 4 + 3];
      t[i * 4 + 0] = a + b + cc + d;
      t[i * 4 + 1] = a + b - cc - d;
      t[i * 4 + 2] = a - b - cc + d;
      t[i * 4 + 3] = a - b + cc - d;
    }
    for (int j = 0; j < 4; ++j) {
      const int a = t[j], b = t[4 + j], cc = t[8 + j], d = t[12 + j];
      f[j] = a + b + cc + d;
      f[4 + j] = a + b - cc - d;
      f[8 + j] = a - b - cc + d;
      f[12 + j] = a - b + cc - d;
    }
    const int ls = 16 * kNorm[qp % 6][0], qpd = qp / 6;
    for (int i = 0; i < 16; ++i) {
      const int64_t v = static_cast<int64_t>(f[i]) * ls;
      dcl[i] = static_cast<int>(qpd >= 6 ? v << (qpd - 6) : (v + (1 << (5 - qpd))) >> (6 - qpd));
    }
  }
  for (int blk = 0; blk < 16; ++blk) {
    const int bx = blk & 3, by = blk >> 2;
    const bool coded = i16 || ((m_.cbp_luma >> ((by >> 1) * 2 + (bx >> 1))) & 1);
    int d[16] = {};
    bool any = false;
    if (coded) {
      for (int i = i16 ? 1 : 0; i < 16; ++i) {
        if (m_.ac[blk][i] == 0) continue;
        const int p = kZigzag4x4[i];
        d[p] = dequant(m_.ac[blk][i], qp, p);
        any = true;
      }
    }
    if (i16) {
      d[0] = dcl[blk];
      any = any || d[0] != 0;
    }
    if (any) inverse4x4_add(d, rec_.y, base + uint64_t{static_cast<uint32_t>(by * 4)} * W + static_cast<uint32_t>(bx * 4), W);
  }
  const uint32_t CW = W / 2;
  const uint64_t cbase = uint64_t{static_cast<uint32_t>(mby)} * 8 * CW + static_cast<uint32_t>(mbx) * 8;
  for (int c = 0; c < 2; ++c) {
    ByteView& p = c == 0 ? rec_.u : rec_.v;
    for (int y = 0; y < 8; ++y)
      for (int x = 0; x < 8; ++x) p[cbase + uint64_t{static_cast<uint32_t>(y)} * CW + static_cast<uint32_t>(x)] = m_.pred_c[c][y * 8 + x];
    if (m_.cbp_chroma == 0) continue;
    const int a = m_.cdc[c][0], b = m_.cdc[c][1], cc = m_.cdc[c][2], dd = m_.cdc[c][3];
    const int f[4] = {a + b + cc + dd, a - b + cc - dd, a + b - cc - dd, a - b - cc + dd};
    const int ls = 16 * kNorm[qpc % 6][0];
    for (int blk = 0; blk < 4; ++blk) {
      int d[16] = {};
      d[0] = static_cast<int>((static_cast<int64_t>(f[blk]) * ls << (qpc / 6)) >> 5);
      if (m_.cbp_chroma == 2) {
        for (int i = 1; i < 16; ++i) {
          if (m_.cac[c][blk][i] == 0) continue;
          const int pos = kZigzag4x4[i];
          d[pos] = dequant(m_.cac[c][blk][i], qpc, pos);
        }
      }
      inverse4x4_add(d, p, cbase + uint64_t{static_cast<uint32_t>((blk >> 1) * 4)} * CW + static_cast<uint32_t>((blk & 1) * 4), CW);
    }
  }
}

// ---------------------------------------------------------------------------------------------------------------
// Syntax

void PictureCoder::write_mb(int mbx, int mby, int qp_delta, const Vec* mvp) {
  BitWriter& w = bw_;
  uint8_t nz[24] = {};
  if (p_slice() || b_slice()) {
    w.ue(skip_run_);
    skip_run_ = 0;
  }
  if (m_.intra) {
    const int type = 1 + m_.i16_mode + 4 * m_.cbp_chroma + (m_.cbp_luma ? 12 : 0);
    w.ue(static_cast<uint32_t>((p_slice() ? 5 : b_slice() ? 23 : 0) + type));
    w.ue(static_cast<uint32_t>(m_.chroma_mode));
    w.se(qp_delta);
    write_block(w, nc_for(0, 0, 0, mbx, mby, nz), m_.dc, 16);
  } else {
    const uint64_t n0 = pic_->list[0].size(), n1 = pic_->list[1].size();
    if (p_slice()) w.ue(0);   // P_L0_16x16
    else w.ue(static_cast<uint32_t>(m_.pred_flags == 1 ? 1 : m_.pred_flags == 2 ? 2 : 3));
    auto te = [&](uint64_t count, int v) {
      if (count == 2) w.bit(v == 0 ? 1 : 0);
      else if (count > 2) w.ue(static_cast<uint32_t>(v));
    };
    if (m_.pred_flags & 1) te(n0, m_.ref[0]);
    if (m_.pred_flags & 2) te(n1, m_.ref[1]);
    for (int l = 0; l < 2; ++l) {
      if (!(m_.pred_flags & (1 << l))) continue;
      w.se(m_.mv[l].x - mvp[l].x);
      w.se(m_.mv[l].y - mvp[l].y);
    }
    const int cbp = m_.cbp_luma | (m_.cbp_chroma << 4);
    int code = 0;
    while (code < 48 && kCbpInter[code] != cbp) ++code;
    w.ue(static_cast<uint32_t>(code));
    if (cbp != 0) w.se(qp_delta);
  }
  if (m_.intra || m_.cbp_luma != 0 || m_.cbp_chroma != 0) {
    for (int b8 = 0; b8 < 4; ++b8) {
      if (!((m_.cbp_luma >> b8) & 1)) continue;
      for (int s = 0; s < 4; ++s) {
        const int bx = (b8 & 1) * 2 + (s & 1), by = (b8 >> 1) * 2 + (s >> 1);
        const int blk = by * 4 + bx;
        const int nc = nc_for(bx, by, 0, mbx, mby, nz);
        nz[blk] = static_cast<uint8_t>(m_.intra ? write_block(w, nc, &m_.ac[blk][1], 15) : write_block(w, nc, m_.ac[blk], 16));
      }
    }
    if (m_.cbp_chroma > 0) {
      for (int c = 0; c < 2; ++c) write_block(w, -1, m_.cdc[c], 4);
      if (m_.cbp_chroma == 2)
        for (int c = 0; c < 2; ++c)
          for (int s = 0; s < 4; ++s) nz[16 + c * 4 + s] = static_cast<uint8_t>(write_block(w, nc_for(s & 1, s >> 1, 1 + c, mbx, mby, nz), &m_.cac[c][s][1], 15));
    }
  }
  for (int i = 0; i < 24; ++i) m_.nz[i] = nz[i];
}

// 7.3.3
void PictureCoder::write_slice_header(uint32_t first_mb) {
  BitWriter& w = bw_;
  const H264EncPicture& p = *pic_;
  const bool idr = p.type == H264EncFrameType::Idr;
  w.ue(first_mb);
  w.ue(p_slice() ? 5 : b_slice() ? 6 : 7);
  w.ue(p.pic_parameter_set_id);
  w.put(p.frame_num & ((1u << p.log2_max_frame_num) - 1), p.log2_max_frame_num);
  if (idr) w.ue(p.idr_pic_id);
  if (p.pic_order_cnt_type == 0) w.put(p.poc & ((1u << p.log2_max_poc_lsb) - 1), p.log2_max_poc_lsb);
  if (b_slice()) w.bit(1);   // direct_spatial_mv_pred_flag (no macroblock is direct)
  if (p_slice() || b_slice()) {
    w.bit(1);   // num_ref_idx_active_override_flag
    w.ue(static_cast<uint32_t>(p.list[0].size() - 1));
    if (b_slice()) w.ue(static_cast<uint32_t>(p.list[1].size() - 1));
    for (int l = 0; l < (b_slice() ? 2 : 1); ++l) {
      uint64_t count = 0;
      for (const H264EncListModification& m : p.modification[l]) {
        if (m.idc == 3) break;
        ++count;
      }
      w.bit(count ? 1 : 0);
      if (!count) continue;
      for (uint64_t i = 0; i < count; ++i) {
        const H264EncListModification& m = p.modification[l][i];
        w.ue(m.idc);
        if (m.idc == 2) w.ue(m.long_term_pic_num);
        else w.ue(m.abs_diff_pic_num_minus1);
      }
      w.ue(3);
    }
  }
  if (p.reference || idr) {
    if (idr) {
      w.bit(0);   // no_output_of_prior_pics_flag
      w.bit(0);   // long_term_reference_flag
    } else {
      w.bit(p.adaptive_ref_pic_marking ? 1 : 0);
      if (p.adaptive_ref_pic_marking) {
        for (const H264EncMarking& m : p.marking) {
          if (m.operation == 0) break;
          w.ue(m.operation);
          if (m.operation == 1 || m.operation == 3) w.ue(m.difference_of_pic_nums_minus1);
          if (m.operation == 2) w.ue(m.long_term_pic_num);
          if (m.operation == 3 || m.operation == 6) w.ue(m.long_term_frame_idx);
          if (m.operation == 4) w.ue(m.max_long_term_frame_idx_plus1);
        }
        w.ue(0);
      }
    }
  }
  w.se(static_cast<int>(p.qp) - 26);   // slice_qp_delta (pic_init_qp 26)
  w.ue(p.disable_deblocking_filter_idc);
  if (p.disable_deblocking_filter_idc != 1) {
    w.se(0);   // slice_alpha_c0_offset_div2
    w.se(0);   // slice_beta_offset_div2
  }
}

// ---------------------------------------------------------------------------------------------------------------
// Macroblocks

void PictureCoder::code_mb(int mbx, int mby) {
  const uint64_t a = addr(mbx, mby);
  MbData& info = mbs_[a];
  info = MbData();
  info.slice = slice_;
  Mb& m = m_;
  // the macroblock's QP
  int qp = pic_->qp;
  if (!pic_->qp_map.empty()) qp = pic_->qp_map_absolute ? pic_->qp_map[a] : qp + pic_->qp_map[a];
  qp = clip3(0, 51, qp);
  m.qp = qp;
  m.qpc = kChromaQp[qp];
  lambda_ = kLambda[qp];

  // intra: the Intra16x16 mode with the least SAD
  uint8_t ipred[256];
  int intra_cost = 0x7FFFFFFF, best_mode = 2;
  for (int mode = 0; mode < 4; ++mode) {
    if (!pred_i16(mode, mbx, mby, ipred)) continue;
    const int c = static_cast<int>(sad16(mbx, mby, ipred)) + lambda_ * (mode == 2 ? 4 : 6);
    if (c < intra_cost) {
      intra_cost = c;
      best_mode = mode;
    }
  }
  bool use_inter = false, skipped = false;
  Vec mvp[2] = {};
  if (p_slice() || b_slice()) {
    int best_cost[2] = {0x7FFFFFFF, 0x7FFFFFFF};
    int best_ref[2] = {0, 0};
    Vec best_mv[2] = {};
    for (int l = 0; l < (b_slice() ? 2 : 1); ++l) {
      for (uint64_t r = 0; r < pic_->list[l].size(); ++r) {
        const Vec pred = predict_mv(mbx, mby, l, static_cast<int>(r));
        Vec v;
        const int c = search(mbx, mby, l, static_cast<int>(r), pred, &v);
        if (c < best_cost[l]) {
          best_cost[l] = c;
          best_ref[l] = static_cast<int>(r);
          best_mv[l] = v;
        }
      }
    }
    int inter_cost;
    if (p_slice()) {
      m.pred_flags = 1;
      inter_cost = best_cost[0] + lambda_ * 1;
      // P_Skip costs next to nothing: taken when its prediction is about as good
      const Vec smv = skip_mv(mbx, mby);
      uint8_t sp[256];
      pred_luma(0, 0, smv, mbx, mby, sp);
      const int skip_cost = static_cast<int>(sad16(mbx, mby, sp));
      if (skip_cost <= inter_cost + lambda_ * 4) {
        best_ref[0] = 0;
        best_mv[0] = smv;
        inter_cost = skip_cost;
      }
    } else {
      const int c0 = best_cost[0] + lambda_ * 3, c1 = best_cost[1] + lambda_ * 3;
      uint8_t p0[256], p1[256];
      pred_luma(0, best_ref[0], best_mv[0], mbx, mby, p0);
      pred_luma(1, best_ref[1], best_mv[1], mbx, mby, p1);
      for (int i = 0; i < 256; ++i) p0[i] = static_cast<uint8_t>((p0[i] + p1[i] + 1) >> 1);
      const Vec pr0 = predict_mv(mbx, mby, 0, best_ref[0]), pr1 = predict_mv(mbx, mby, 1, best_ref[1]);
      const int cb = static_cast<int>(sad16(mbx, mby, p0)) + mv_cost(best_mv[0], pr0) + mv_cost(best_mv[1], pr1) + ref_cost(0, best_ref[0]) + ref_cost(1, best_ref[1]) +
                     lambda_ * 5;
      m.pred_flags = cb <= c0 && cb <= c1 ? 3 : c0 <= c1 ? 1 : 2;
      inter_cost = m.pred_flags == 3 ? cb : m.pred_flags == 1 ? c0 : c1;
    }
    use_inter = inter_cost <= intra_cost;
    if (use_inter) {
      for (int l = 0; l < 2; ++l) {
        m.ref[l] = best_ref[l];
        m.mv[l] = best_mv[l];
        if (m.pred_flags & (1 << l)) mvp[l] = predict_mv(mbx, mby, l, best_ref[l]);
      }
    }
  }
  m.intra = !use_inter;
  if (m.intra) {
    m.i16_mode = best_mode;
    pred_i16(best_mode, mbx, mby, m.pred_y);
    uint32_t best = 0xFFFFFFFFu;
    m.chroma_mode = 0;
    for (int mode = 0; mode < 4; ++mode) {
      uint8_t cp[2][64];
      if (!pred_chroma(mode, mbx, mby, 0, cp[0]) || !pred_chroma(mode, mbx, mby, 1, cp[1])) continue;
      uint32_t s = 0;
      const uint32_t CW = src_.width / 2;
      const uint64_t base = uint64_t{static_cast<uint32_t>(mby)} * 8 * CW + static_cast<uint32_t>(mbx) * 8;
      for (int c = 0; c < 2; ++c) {
        const ConstByteView& sp = c == 0 ? src_.u : src_.v;
        for (int y = 0; y < 8; ++y)
          for (int x = 0; x < 8; ++x) s += static_cast<uint32_t>(iabs(int{sp[base + uint64_t{static_cast<uint32_t>(y)} * CW + static_cast<uint32_t>(x)]} - cp[c][y * 8 + x]));
      }
      if (s < best) {
        best = s;
        m.chroma_mode = mode;
      }
    }
    pred_chroma(m.chroma_mode, mbx, mby, 0, m.pred_c[0]);
    pred_chroma(m.chroma_mode, mbx, mby, 1, m.pred_c[1]);
    quant_luma_i16(mbx, mby);
    quant_chroma(mbx, mby, true);
  } else {
    if (m.pred_flags == 3) {
      uint8_t p1[256], c1[2][64];
      pred_luma(0, m.ref[0], m.mv[0], mbx, mby, m.pred_y);
      pred_luma(1, m.ref[1], m.mv[1], mbx, mby, p1);
      pred_chroma_inter(0, m.ref[0], m.mv[0], mbx, mby, m.pred_c);
      pred_chroma_inter(1, m.ref[1], m.mv[1], mbx, mby, c1);
      for (int i = 0; i < 256; ++i) m.pred_y[i] = static_cast<uint8_t>((m.pred_y[i] + p1[i] + 1) >> 1);
      for (int c = 0; c < 2; ++c)
        for (int i = 0; i < 64; ++i) m.pred_c[c][i] = static_cast<uint8_t>((m.pred_c[c][i] + c1[c][i] + 1) >> 1);
    } else {
      const int l = m.pred_flags == 1 ? 0 : 1;
      pred_luma(l, m.ref[l], m.mv[l], mbx, mby, m.pred_y);
      pred_chroma_inter(l, m.ref[l], m.mv[l], mbx, mby, m.pred_c);
    }
    quant_luma_inter(mbx, mby);
    quant_chroma(mbx, mby, false);
    skipped = p_slice() && m.ref[0] == 0 && m.mv[0] == skip_mv(mbx, mby) && m.cbp_luma == 0 && m.cbp_chroma == 0;
  }
  // The QP the macroblock has: a delta is coded only with residual (an Intra16x16 macroblock always has one); without, it keeps the one before.
  const bool coded_qp = m.intra || m.cbp_luma != 0 || m.cbp_chroma != 0;
  int delta = 0;
  if (coded_qp) {
    delta = m.qp - qp_prev_;
    if (delta > 25) delta -= 52;
    if (delta < -26) delta += 52;
  } else {
    m.qp = qp_prev_;
    m.qpc = kChromaQp[m.qp];
  }
  reconstruct(mbx, mby);
  if (skipped) {
    ++skip_run_;
    for (uint8_t& n : m.nz) n = 0;
    ++stats_.skipped;
  } else {
    write_mb(mbx, mby, delta, mvp);
    if (m.intra) ++stats_.intra;
    else ++stats_.inter;
  }
  qp_prev_ = m.qp;
  info.intra = m.intra;
  info.qp = static_cast<uint8_t>(m.qp);
  if (!m.intra) {
    for (int l = 0; l < 2; ++l) {
      if (!(m.pred_flags & (1 << l))) continue;
      info.ref[l] = static_cast<int8_t>(m.ref[l]);
      info.ident[l] = pic_->list[l][static_cast<uint64_t>(m.ref[l])].identity;
      info.mv[l][0] = static_cast<int16_t>(m.mv[l].x);
      info.mv[l][1] = static_cast<int16_t>(m.mv[l].y);
    }
    const int l = (m.pred_flags & 1) ? 0 : 1;
    stats_.vector_x_sum += m.mv[l].x;
    stats_.vector_y_sum += m.mv[l].y;
  }
  for (int i = 0; i < 24; ++i) info.nz[i] = m.nz[i];
  info.nzmask = 0;
  for (int i = 0; i < 16; ++i)
    if (m.nz[i]) info.nzmask = static_cast<uint16_t>(info.nzmask | (1u << i));
  stats_.qp_sum += static_cast<uint64_t>(m.qp);
}

// ---------------------------------------------------------------------------------------------------------------
// The in-loop filter (8.7), once the picture is coded

// 8.7.2.1: the strength of the edge between 4x4 block blk_p of p and blk_q of q.
int PictureCoder::boundary_strength(const MbData& p, const MbData& q, int blk_p, int blk_q, bool mb_edge) const {
  if (p.intra || q.intra) return mb_edge ? 4 : 3;
  if (((p.nzmask >> blk_p) & 1) || ((q.nzmask >> blk_q) & 1)) return 2;
  int64_t rp[2], rq[2];
  for (int l = 0; l < 2; ++l) {
    rp[l] = p.ref[l] >= 0 ? p.ident[l] : -1;
    rq[l] = q.ref[l] >= 0 ? q.ident[l] : -1;
  }
  const int np = (rp[0] >= 0) + (rp[1] >= 0), nq = (rq[0] >= 0) + (rq[1] >= 0);
  if (np != nq) return 1;
  auto far = [](const int16_t* a, const int16_t* b) { return iabs(a[0] - b[0]) >= 4 || iabs(a[1] - b[1]) >= 4; };
  const int16_t* vp[2] = {p.mv[0], p.mv[1]};
  const int16_t* vq[2] = {q.mv[0], q.mv[1]};
  if (np == 1) {
    const int lp = rp[0] >= 0 ? 0 : 1, lq = rq[0] >= 0 ? 0 : 1;
    if (rp[lp] != rq[lq]) return 1;
    return far(vp[lp], vq[lq]) ? 1 : 0;
  }
  if (np == 0) return 0;
  if (!((rp[0] == rq[0] && rp[1] == rq[1]) || (rp[0] == rq[1] && rp[1] == rq[0]))) return 1;
  if (rp[0] != rp[1]) {
    if (rp[0] == rq[0]) return (far(vp[0], vq[0]) || far(vp[1], vq[1])) ? 1 : 0;
    return (far(vp[0], vq[1]) || far(vp[1], vq[0])) ? 1 : 0;
  }
  return ((far(vp[0], vq[0]) || far(vp[1], vq[1])) && (far(vp[0], vq[1]) || far(vp[1], vq[0]))) ? 1 : 0;
}

void PictureCoder::deblock_mb(int mbx, int mby) {
  const uint8_t idc = pic_->disable_deblocking_filter_idc;
  if (idc == 1) return;
  const MbData& c = mbs_[addr(mbx, mby)];
  const bool left_ok = mbx > 0 && !(idc == 2 && mbs_[addr(mbx - 1, mby)].slice != c.slice);
  const bool top_ok = mby > 0 && !(idc == 2 && mbs_[addr(mbx, mby - 1)].slice != c.slice);
  int bs[2][4][4];
  for (int dir = 0; dir < 2; ++dir) {
    for (int e = 0; e < 4; ++e) {
      for (int s = 0; s < 4; ++s) {
        bs[dir][e][s] = 0;
        if (e == 0 && !(dir == 0 ? left_ok : top_ok)) continue;
        const MbData& p = e == 0 ? (dir == 0 ? mbs_[addr(mbx - 1, mby)] : mbs_[addr(mbx, mby - 1)]) : c;
        const int blk_q = dir == 0 ? s * 4 + e : e * 4 + s;
        const int blk_p = dir == 0 ? (e == 0 ? s * 4 + 3 : s * 4 + e - 1) : (e == 0 ? 12 + s : (e - 1) * 4 + s);
        bs[dir][e][s] = boundary_strength(p, c, blk_p, blk_q, e == 0);
      }
    }
  }
  const int64_t ls = rec_.width;
  const int64_t ly = static_cast<int64_t>(mby) * 16 * ls + mbx * 16;
  for (int dir = 0; dir < 2; ++dir) {
    for (int e = 0; e < 4; ++e) {
      if (e == 0 && !(dir == 0 ? left_ok : top_ok)) continue;
      const MbData& p = e == 0 ? (dir == 0 ? mbs_[addr(mbx - 1, mby)] : mbs_[addr(mbx, mby - 1)]) : c;
      const int qpav = (p.qp + c.qp + 1) >> 1;
      const int index_a = clip3(0, 51, qpav), index_b = clip3(0, 51, qpav);
      const int alpha = kAlpha[index_a], beta = kBeta[index_b];
      if (alpha == 0 || beta == 0) continue;
      filter_edge(rec_.y, dir == 0 ? ly + e * 4 : ly + e * 4 * ls, dir == 0 ? 1 : ls, dir == 0 ? ls : 1, 16, bs[dir][e], index_a, alpha, beta, false, 4);
    }
  }
  const int64_t cs = rec_.width / 2;
  for (int plane = 0; plane < 2; ++plane) {
    ByteView pl = plane == 0 ? rec_.u : rec_.v;
    const int64_t base = static_cast<int64_t>(mby) * 8 * cs + mbx * 8;
    for (int dir = 0; dir < 2; ++dir) {
      for (int e = 0; e < 2; ++e) {
        if (e == 0 && !(dir == 0 ? left_ok : top_ok)) continue;
        const MbData& p = e == 0 ? (dir == 0 ? mbs_[addr(mbx - 1, mby)] : mbs_[addr(mbx, mby - 1)]) : c;
        const int qpav = (kChromaQp[p.qp] + kChromaQp[c.qp] + 1) >> 1;
        const int index_a = clip3(0, 51, qpav), index_b = clip3(0, 51, qpav);
        const int alpha = kAlpha[index_a], beta = kBeta[index_b];
        if (alpha == 0 || beta == 0) continue;
        filter_edge(pl, dir == 0 ? base + e * 4 : base + e * 4 * cs, dir == 0 ? 1 : cs, dir == 0 ? cs : 1, 8, bs[dir][e * 2], index_a, alpha, beta, true, 2);
      }
    }
  }
}

// ---------------------------------------------------------------------------------------------------------------
// Pictures

H264EncResult PictureCoder::encode(const H264EncPicture& pic, const H264EncPlanes& src, const H264EncWritablePlanes& rec, ByteBuffer* out, List<H264EncSlice>* slices,
                                   H264EncStatistics* stats) {
  pic_ = &pic;
  mbw_ = pic.mb_width;
  mbh_ = pic.mb_height;
  const uint64_t mbs = uint64_t{mbw_} * mbh_;
  const uint32_t W = mbw_ * 16, H = mbh_ * 16;
  // (what the picture says must be what the slice syntax can say)
  if (mbw_ == 0 || mbh_ == 0 || mbw_ > 1024 || mbh_ > 1024 || pic.disable_deblocking_filter_idc > 2 || pic.log2_max_frame_num < 4 || pic.log2_max_frame_num > 16 ||
      (pic.pic_order_cnt_type != 0 && pic.pic_order_cnt_type != 2) || pic.log2_max_poc_lsb < 4 || pic.log2_max_poc_lsb > 16 || pic.qp > 51 ||
      static_cast<uint8_t>(pic.type) > 3 || pic.pic_parameter_set_id > 255 || pic.idr_pic_id > 65535)
    return H264EncResult::PictureControlNotSupported;
  const bool inter = pic.type == H264EncFrameType::P || pic.type == H264EncFrameType::B;
  if ((inter && pic.list[0].empty()) || (pic.type == H264EncFrameType::B && pic.list[1].empty()) || pic.list[0].size() > 16 || pic.list[1].size() > 16 ||
      (!inter && (!pic.list[0].empty() || !pic.list[1].empty())) || (pic.type == H264EncFrameType::P && !pic.list[1].empty()))
    return H264EncResult::PictureControlNotSupported;
  if (pic.type == H264EncFrameType::B && pic.pic_order_cnt_type == 2) return H264EncResult::PictureControlNotSupported;   // (output order is decoding order there)
  if (!pic.qp_map.empty() && pic.qp_map.size() != mbs) return H264EncResult::PictureControlNotSupported;
  for (const int8_t q : pic.qp_map)
    if (pic.qp_map_absolute ? (q < 0 || q > 51) : (q < -51 || q > 51)) return H264EncResult::PictureControlNotSupported;
  for (const H264EncMarking& m : pic.marking)
    if (m.operation > 6) return H264EncResult::PictureControlNotSupported;
  for (int l = 0; l < 2; ++l)
    for (const H264EncListModification& m : pic.modification[l])
      if (m.idc > 3) return H264EncResult::PictureControlNotSupported;
  auto planes_fit = [&](uint64_t y, uint64_t u, uint64_t v, uint32_t w, uint32_t h) {
    return w == W && h == H && y >= uint64_t{W} * H && u >= uint64_t{W} * H / 4 && v >= uint64_t{W} * H / 4;
  };
  if (!planes_fit(src.y.size(), src.u.size(), src.v.size(), src.width, src.height) || !planes_fit(rec.y.size(), rec.u.size(), rec.v.size(), rec.width, rec.height))
    return H264EncResult::InvalidReferences;
  for (int l = 0; l < 2; ++l)
    for (const H264EncReference& r : pic.list[l])
      if (!planes_fit(r.picture.y.size(), r.picture.u.size(), r.picture.v.size(), r.picture.width, r.picture.height)) return H264EncResult::InvalidReferences;
  const uint32_t count = H264PictureEncoder::slice_count(pic.slice_mode, pic.slice_value, mbw_, mbh_);
  if (count == 0) return H264EncResult::SubregionLayoutNotSupported;
  starts_.clear();
  for (uint32_t s = 0; s <= count; ++s) {
    uint64_t start;
    switch (pic.slice_mode) {
      case H264EncSliceMode::MacroblocksPerSlice: start = uint64_t{s} * pic.slice_value; break;
      case H264EncSliceMode::RowsPerSlice: start = uint64_t{s} * pic.slice_value * mbw_; break;
      case H264EncSliceMode::SlicesPerFrame: start = uint64_t{s} * mbs / count; break;
      default: start = s == 0 ? 0 : mbs;
    }
    if (!starts_.push_back(static_cast<uint32_t>(start < mbs ? start : mbs))) return H264EncResult::NoMemory;
  }
  mbs_.clear();
  if (!mbs_.reserve(mbs)) return H264EncResult::NoMemory;
  for (uint64_t i = 0; i < mbs; ++i)
    if (!mbs_.push_back(MbData())) return H264EncResult::NoMemory;
  src_ = src;
  rec_ = rec;
  stats_ = H264EncStatistics();
  slices->clear();
  const uint64_t mark = out->size();
  const bool idr = pic.type == H264EncFrameType::Idr;
  const int ref_idc = !(pic.reference || idr) ? 0 : (idr || pic.type == H264EncFrameType::I) ? 3 : 2;
  for (uint32_t s = 0; s < count; ++s) {
    slice_ = static_cast<int32_t>(s);
    bw_.reset();
    write_slice_header(starts_[s]);
    const uint64_t header_bits = bw_.bits();
    qp_prev_ = pic.qp;
    skip_run_ = 0;
    for (uint32_t a = starts_[s]; a < starts_[s + 1]; ++a) code_mb(static_cast<int>(a % mbw_), static_cast<int>(a / mbw_));
    if (skip_run_ > 0) bw_.ue(skip_run_);
    bw_.trailing();
    if (!bw_.ok()) {
      out->truncate(mark);
      return H264EncResult::NoMemory;
    }
    // the NAL unit: start code, header, the RBSP with emulation prevention bytes
    H264EncSlice slice;
    slice.offset = out->size();
    static const uint8_t kStart[4] = {0, 0, 0, 1};
    bool ok = out->append(ConstByteView(kStart, 4)) && out->put8(static_cast<uint8_t>((ref_idc << 5) | (idr ? 5 : 1)));
    int zeros = 0;
    uint64_t index = 0, inserted_in_header = 0;
    const uint64_t header_bytes = (header_bits + 7) / 8;
    for (const uint8_t b : bw_.bytes()) {
      if (zeros >= 2 && b <= 3) {
        ok = out->put8(3) && ok;
        zeros = 0;
        if (index < header_bytes) ++inserted_in_header;
      }
      ok = out->put8(b) && ok;
      zeros = b == 0 ? zeros + 1 : 0;
      ++index;
    }
    slice.size = out->size() - slice.offset;
    slice.header_bits = 40 + header_bits + 8 * inserted_in_header;
    if (!ok || !slices->push_back(slice)) {
      out->truncate(mark);
      return H264EncResult::NoMemory;
    }
  }
  for (uint32_t y = 0; y < mbh_; ++y)
    for (uint32_t x = 0; x < mbw_; ++x) deblock_mb(static_cast<int>(x), static_cast<int>(y));
  if (stats) *stats = stats_;
  return H264EncResult::Ok;
}

}  // namespace h264

H264PictureEncoder::H264PictureEncoder(allocators::IAllocator& allocator) : allocator_(allocator) {}
H264PictureEncoder::~H264PictureEncoder() {}

uint32_t H264PictureEncoder::slice_count(H264EncSliceMode mode, uint32_t value, uint32_t mb_width, uint32_t mb_height) {
  const uint64_t mbs = uint64_t{mb_width} * mb_height;
  if (mbs == 0) return 0;
  switch (mode) {
    case H264EncSliceMode::FullFrame: return 1;
    case H264EncSliceMode::MacroblocksPerSlice: return value == 0 ? 0 : static_cast<uint32_t>((mbs + value - 1) / value);
    case H264EncSliceMode::RowsPerSlice: return value == 0 ? 0 : (mb_height + value - 1) / value;
    case H264EncSliceMode::SlicesPerFrame: return value == 0 || value > mbs ? 0 : value;
    default: return 0;   // (BytesPerSlice is not done)
  }
}

H264EncResult H264PictureEncoder::encode(const H264EncPicture& picture, const H264EncPlanes& source, const H264EncWritablePlanes& reconstruction, ByteBuffer* out,
                                         List<H264EncSlice>* slices, H264EncStatistics* statistics) {
  if (!core_) {
    core_ = UniquePointer<h264::PictureCoder>::make(allocator_, allocator_);
    if (!core_) return H264EncResult::NoMemory;
  }
  return core_->encode(picture, source, reconstruction, out, slices, statistics);
}

}  // namespace hardware
