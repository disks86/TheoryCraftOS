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

// The H.265 picture encoder (H265PictureEncoder.hpp). Clause numbers are ITU-T H.265's.
//
// A picture is coded in three steps:
//   1. decisions: coding tree block by coding tree block, the coding quadtree, the modes, the motion and the quantized coefficients are chosen by cost (distortion of
//      the reconstruction plus lambda times an estimate of the bits), with a reconstruction before the in-loop filters that is what a decoder makes at the same
//      point (the prediction, scaling and inverse transforms follow the decoder, H265Predict.cpp, exactly; the motion vector prediction and merge candidates follow
//      H265Slice.cpp). Decisions are kept per 8x8 cell and the coefficients in planes the size of the picture.
//   2. entropy coding: the slice segment NAL units, CABAC as the decoder reads it (H265Slice.cpp is mirrored: the same contexts and binarizations).
//   3. the reconstruction: the slices are decoded by the in-tree decoder (h265::Core's external interface, with the program's references), which applies the deblocking
//      filter and SAO. With SAO on, the slices are first coded without SAO, decoded, the SAO parameters chosen from that deblocked picture, and coded again.
#include "Hardware/VideoCodec/Software/H265PictureEncoder.hpp"
#include "Hardware/VideoCodec/Software/H265Core.hpp"
#include "Hardware/VideoCodec/Software/MotionEstimator.hpp"
#include "Utility/AllocatedView.hpp"

namespace hardware {
namespace h265 {

namespace {

constexpr int clip3(int lo, int hi, int v) { return v < lo ? lo : v > hi ? hi : v; }
constexpr int imin(int a, int b) { return a < b ? a : b; }
constexpr int imax(int a, int b) { return a > b ? a : b; }
constexpr int iabs(int a) { return a < 0 ? -a : a; }
constexpr int64_t kInfinite = int64_t{1} << 60;

int ilog2(uint32_t v) {
  int n = 0;
  while (v > 1) {
    v >>= 1;
    ++n;
  }
  return n;
}
int ceil_log2(uint32_t v) {
  int n = 0;
  while ((1u << n) < v) ++n;
  return n;
}

// (H265Predict.cpp)
const int8_t kTransform[32][32] =
#include "Hardware/VideoCodec/Software/H265Transform.inc"
    ;
const int8_t kIntraPredAngle[35] = {0, 0, 32, 26, 21, 17, 13, 9, 5, 2, 0, -2, -5, -9, -13, -17, -21, -26, -32, -26, -21, -17, -13, -9, -5, -2, 0, 2, 5, 9, 13, 17, 21, 26, 32};
const int16_t kInvAngle[35] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -4096, -1638, -910, -630, -482, -390, -315, -256, -315, -390, -482, -630, -910, -1638, -4096, 0, 0, 0, 0, 0, 0, 0, 0, 0};
const int8_t kLumaFilter[4][8] = {{0, 0, 0, 64, 0, 0, 0, 0}, {-1, 4, -10, 58, 17, -5, 1, 0}, {-1, 4, -11, 40, 40, -11, 4, -1}, {0, 1, -5, 17, 58, -10, 4, -1}};
const int8_t kChromaFilter[8][4] = {{0, 64, 0, 0}, {-2, 58, 10, -2}, {-4, 54, 16, -2}, {-6, 46, 28, -4}, {-4, 36, 36, -4}, {-4, 28, 46, -6}, {-2, 16, 54, -4}, {-2, 10, 58, -2}};
const int kLevelScale[6] = {40, 45, 51, 57, 64, 72};
const int kQuantScale[6] = {26214, 23302, 20560, 18396, 16384, 14564};

// (H265Slice.cpp) scan orders of 1x1 .. 8x8 positions: type 0 up-right diagonal, 1 horizontal, 2 vertical.
struct ScanTable {
  uint8_t x[3][4][64];
  uint8_t y[3][4][64];
};
constexpr ScanTable make_scans() {
  ScanTable t{};
  for (int log2 = 0; log2 < 4; ++log2) {
    const int n = 1 << log2;
    int i = 0, x = 0, y = 0;
    bool stop = false;
    while (!stop) {
      while (y >= 0) {
        if (x < n && y < n) {
          t.x[0][log2][i] = static_cast<uint8_t>(x);
          t.y[0][log2][i] = static_cast<uint8_t>(y);
          ++i;
        }
        --y;
        ++x;
      }
      y = x;
      x = 0;
      if (i >= n * n) stop = true;
    }
    for (int k = 0; k < n * n; ++k) {
      t.x[1][log2][k] = static_cast<uint8_t>(k % n);
      t.y[1][log2][k] = static_cast<uint8_t>(k / n);
      t.x[2][log2][k] = static_cast<uint8_t>(k / n);
      t.y[2][log2][k] = static_cast<uint8_t>(k % n);
    }
  }
  return t;
}
constexpr ScanTable kScan = make_scans();
constexpr uint8_t kCtxIdxMap4x4[16] = {0, 1, 4, 5, 2, 3, 4, 5, 6, 6, 8, 8, 7, 7, 8, 8};

// Lambda (x 256) of 0.57 * 2^((QP - 12) / 3) for QP -12 .. 51, and 16 x its square root (for costs of sums of absolute differences).
const int32_t kLambda[64] = {1, 1, 1, 1, 1, 2, 2, 3, 4, 5, 6, 7, 9, 11, 14, 18, 23, 29, 36, 46, 58, 73, 92, 116, 146, 184, 232, 292, 368, 463, 584, 735, 927, 1167, 1471, 1853,
                             2335, 2942, 3706, 4669, 5883, 7412, 9339, 11766, 14825, 18678, 23533, 29649, 37356, 47065, 59298, 74711, 94130, 118596, 149422, 188260,
                             237193, 298844, 376520, 474386, 597688, 753040, 948771, 1195377};
const int32_t kSqrtLambda[64] = {1,  1,  1,  1,  1,  1,  2,  2,  2,  2,  2,  3,  3,  3,  4,  4,  5,  5,  6,  7,  8,  9,  10, 11, 12, 14, 15, 17, 19, 22, 24, 27,
                                 30, 34, 38, 43, 48, 54, 61, 68, 77, 86, 97, 108, 122, 137, 153, 172, 193, 217, 244, 273, 307, 344, 387, 434, 487, 547, 614, 689, 773, 868, 974, 1093};

enum : int { kPredL0 = 0, kPredL1 = 1, kPredBi = 2 };
enum : uint8_t { kModeIntra = 1, kModeInter = 2, kModeSkip = 4 };

// ---- bits ----------------------------------------------------------------------------------------------------------------------------------------------------------

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
    align_zero();
  }
  void align_zero() {
    while (count_ != 0) bit(0);
  }
  uint64_t bits() const { return buf_.size() * 8 + static_cast<uint64_t>(count_); }
  bool ok() const { return ok_; }
  ConstByteView bytes() const { return buf_.view(); }

 private:
  ByteBuffer buf_;
  uint8_t acc_ = 0;
  int count_ = 0;
  bool ok_ = true;
};

// A NAL unit (Annex B, four-byte start code) of the two header bytes and the RBSP, with emulation prevention. *header_out (if given) is the size in bytes of the start code,
// the header and the first `rbsp_header` bytes of the RBSP as written (emulation prevention bytes included).
bool put_nal(ByteBuffer* out, int type, int temporal_id, ConstByteView rbsp, uint64_t rbsp_header, uint64_t* header_out) {
  const uint64_t start = out->size();
  bool ok = out->put8(0) && out->put8(0) && out->put8(0) && out->put8(1);
  ok = ok && out->put8(static_cast<uint8_t>(type << 1)) && out->put8(static_cast<uint8_t>(temporal_id + 1));
  int zeros = 0;
  for (uint64_t i = 0; i < rbsp.size(); ++i) {
    if (i == rbsp_header && header_out) *header_out = out->size() - start;
    const uint8_t b = rbsp[i];
    if (zeros >= 2 && b <= 3) {
      ok = ok && out->put8(3);
      zeros = 0;
    }
    ok = ok && out->put8(b);
    zeros = b == 0 ? zeros + 1 : 0;
  }
  if (header_out && rbsp_header >= rbsp.size()) *header_out = out->size() - start;
  return ok;
}

// The CABAC arithmetic encoder (9.3.4.3 in reverse; the encoding process of ITU-T H.264 9.3.4.2, which H.265 shares).
class CabacWriter {
 public:
  void start(BitWriter* w, int init_type, int slice_qp) {
    w_ = w;
    low_ = 0;
    range_ = 510;
    outstanding_ = 0;
    first_ = true;
    Cabac c;
    c.init_contexts(init_type, slice_qp);
    c.save(state_);
  }
  void decision(int ctx, int bin) {
    uint8_t& st = state_[ctx];
    int state = st >> 1, mps = st & 1;
    const uint32_t lps = kCabacRangeLps[state * 4 + ((range_ >> 6) & 3)];
    range_ -= lps;
    if (bin != mps) {
      low_ += range_;
      range_ = lps;
      if (state == 0) mps = 1 - mps;
      state = kCabacStateNext[state * 2];
    } else {
      state = kCabacStateNext[state * 2 + 1];
    }
    st = static_cast<uint8_t>((state << 1) | mps);
    renorm();
  }
  void bypass(int bin) {
    low_ <<= 1;
    if (bin) low_ += range_;
    if (low_ >= 1024) {
      put_bit(1);
      low_ -= 1024;
    } else if (low_ < 512) {
      put_bit(0);
    } else {
      low_ -= 512;
      ++outstanding_;
    }
  }
  void bypass_bits(uint32_t v, int n) {
    for (int i = n - 1; i >= 0; --i) bypass((v >> i) & 1);
  }
  void terminate(int bin) {
    range_ -= 2;
    if (bin) {
      low_ += range_;
      // flush (9.3.4.3.5): the last bit written is the rbsp_stop_one_bit
      range_ = 2;
      renorm();
      put_bit((low_ >> 9) & 1);
      w_->put(((low_ >> 7) & 3) | 1, 2);
    } else {
      renorm();
    }
  }

 private:
  void renorm() {
    while (range_ < 256) {
      if (low_ < 256) {
        put_bit(0);
      } else if (low_ >= 512) {
        low_ -= 512;
        put_bit(1);
      } else {
        low_ -= 256;
        ++outstanding_;
      }
      range_ <<= 1;
      low_ <<= 1;
    }
  }
  void put_bit(int b) {
    if (first_) first_ = false;
    else w_->bit(static_cast<uint32_t>(b));
    while (outstanding_ > 0) {
      w_->bit(static_cast<uint32_t>(1 - b));
      --outstanding_;
    }
  }
  BitWriter* w_ = nullptr;
  uint32_t low_ = 0, range_ = 510;
  uint32_t outstanding_ = 0;
  bool first_ = true;
  uint8_t state_[kContextCount] = {};
};

// An owned, zero-filled array from an allocator (plain data).
template <class T>
class Array {
 public:
  explicit Array(allocators::IAllocator& allocator) : allocator_(allocator) {}
  ~Array() { release_view(allocator_, view_); }
  Array(const Array&) = delete;
  Array& operator=(const Array&) = delete;
  [[nodiscard]] bool reset(uint64_t count) {
    if (view_.size() != count) {
      release_view(allocator_, view_);
      if (count == 0) return true;
      view_ = allocate_view<T>(allocator_, count);
      if (view_.empty()) return false;
    }
    view_.fill(T{});
    return true;
  }
  ArrayView<T> view() const { return view_; }
  T& operator[](uint64_t i) const { return view_[i]; }
  uint64_t size() const { return view_.size(); }

 private:
  allocators::IAllocator& allocator_;
  ArrayView<T> view_;
};

// (H265Slice.cpp)
bool same_motion(const MvField& a, const MvField& b) {
  if (a.pred_flags != b.pred_flags) return false;
  for (int l = 0; l < 2; ++l) {
    if (!(a.pred_flags & (1 << l))) continue;
    if (a.ref_idx[l] != b.ref_idx[l] || a.mv[l][0] != b.mv[l][0] || a.mv[l][1] != b.mv[l][1]) return false;
  }
  return true;
}
int poc_diff(int a, int b) {
  const int64_t d = static_cast<int64_t>(a) - b;
  return static_cast<int>(d < -32768 ? -32768 : d > 32767 ? 32767 : d);
}
void scale_mv(int td, int tb, const int16_t in[2], int out[2]) {
  td = clip3(-128, 127, td);
  tb = clip3(-128, 127, tb);
  if (td == 0) {
    out[0] = in[0];
    out[1] = in[1];
    return;
  }
  const int tx = (16384 + iabs(td) / 2) / td;
  const int dist = clip3(-4096, 4095, (tb * tx + 32) >> 6);
  for (int i = 0; i < 2; ++i) {
    const int p = dist * in[i];
    out[i] = clip3(-32768, 32767, (p < 0 ? -1 : 1) * ((iabs(p) + 127) >> 8));
  }
}

// What the decisions keep of a coding unit (at the cell of its top-left corner).
struct CuInfo {
  bool intra = false, skip = false, merge = false;
  uint8_t merge_idx = 0, intra_mode = 1;
  uint8_t pred_idc = kPredL0;
  int8_t ref_idx[2] = {-1, -1};
  int16_t mvd[2][2] = {};
  uint8_t mvp[2] = {};
};
struct Cell {
  uint8_t depth = 0, log2 = 3, mode = 0, intra_mode = 1;
  MvField mv{};
  CuInfo cu;
};

// SAO parameters of a coding tree block as coded (7.3.8.3): offsets as sao_offset_abs with their signs (band) or with the signs edge offsets imply.
struct EncSao {
  uint8_t type[3] = {0, 0, 0};
  uint8_t band[3] = {0, 0, 0};
  uint8_t eo_class[3] = {0, 0, 0};
  int8_t offset[3][4] = {};
  bool merge_left = false, merge_up = false;
  bool same(const EncSao& o) const {
    for (int c = 0; c < 3; ++c) {
      if (type[c] != o.type[c]) return false;
      if (type[c] == 0) continue;
      if ((type[c] == 1 && band[c] != o.band[c]) || (type[c] == 2 && eo_class[c] != o.eo_class[c])) return false;
      for (int i = 0; i < 4; ++i)
        if (offset[c][i] != o.offset[c][i]) return false;
    }
    return true;
  }
};

// A list entry: a picture of the dpb.
struct RefEntry {
  int dpb = -1;
  int poc = 0;
  bool long_term = false;
};

constexpr int kMaxCtb = 64;
// A coding unit's area as it was: samples, coefficients and cells (the rate-distortion search keeps the best candidate of each depth).
struct RegionSave {
  uint16_t rec[3][kMaxCtb * kMaxCtb];
  int16_t coef[3][kMaxCtb * kMaxCtb];
  Cell cells[(kMaxCtb / 8) * (kMaxCtb / 8)];
};

}  // namespace

class PictureEncoder {
 public:
  explicit PictureEncoder(allocators::IAllocator& allocator)
      : allocator_(allocator), rec_{Array<uint16_t>(allocator), Array<uint16_t>(allocator), Array<uint16_t>(allocator)},
        coef_{Array<int16_t>(allocator), Array<int16_t>(allocator), Array<int16_t>(allocator)}, cells_(allocator), ctb_qp_(allocator), ctb_sao_(allocator),
        luma8_(allocator), motion_(allocator), slice_bounds_(allocator), nal_(allocator), pass_(allocator), slices_tmp_(allocator), core_(UniquePointer<Core>::make(allocator, allocator)) {}

  H265EncResult encode(const H265EncPicture& pic, const EncPlanes16& src, const EncWritablePlanes16& rec, ByteBuffer* out, List<H265EncSlice>* slices, EncStatistics* stats);
  uint64_t drift() const { return drift_; }
  bool check_drift = false;

 private:
  // set-up
  H265EncResult prepare(const H265EncPicture& pic, const EncPlanes16& src);
  int zorder(int cx, int cy) const {
    const int s = log2_ctb_ - 3, m = (1 << s) - 1;
    const int ctb = (cy >> s) * ctb_w_ + (cx >> s);
    int lx = cx & m, ly = cy & m, z = 0;
    for (int b = 0; b < s; ++b) z |= (((lx >> b) & 1) << (2 * b)) | (((ly >> b) & 1) << (2 * b + 1));
    return (ctb << (2 * s)) + z;
  }
  Cell& cell(int x, int y) const { return cells_[static_cast<uint64_t>(y >> 3) * w8_ + (x >> 3)]; }
  bool available(int xc, int yc, int xn, int yn) const {
    if (xn < 0 || yn < 0 || xn >= W_ || yn >= H_) return false;
    if (zorder(xn >> 3, yn >> 3) > zorder(xc >> 3, yc >> 3)) return false;
    const int ctb_n = (yn >> log2_ctb_) * ctb_w_ + (xn >> log2_ctb_);
    return ctb_n >= slice_start_;
  }
  bool available_pb(int x_cb, int y_cb, int n_cb, int x_pb, int y_pb, int xn, int yn) const;
  // prediction
  void predict_intra(int x0, int y0, int log2, int c, int mode, int* out) const;
  void predict_inter(int x0, int y0, int w, int h, const MvField& m, int* out_y, int* out_u, int* out_v) const;
  void predict_luma(const RefEntry& r, int x0, int y0, int w, int h, int mvx, int mvy, int* out) const;
  void merge_candidates(int x0, int y0, int n, MvField* cands) const;
  void amvp(int x0, int y0, int n, int list, int ref_idx, int out[2][2]) const;
  // residual
  int64_t code_block(int c, int x, int y, int log2, const int* pred, int qp, bool intra, int* bits, bool* nonzero);
  // decisions
  int64_t decide_cu(int x, int y, int log2, int depth);
  int64_t eval_intra(int x, int y, int log2, int depth, int mode, int mode_bits);
  int64_t eval_inter(int x, int y, int log2, int depth, const CuInfo& info, const MvField& m, int syntax_bits, bool residual);
  void set_cu_cells(int x, int y, int log2, int depth, uint8_t mode, int intra_mode, const MvField& m, const CuInfo& info);
  int64_t search(int x0, int y0, int n, int list, int ref_idx, const int pred[2][2], int mv[2]);
  void save_region(int x, int y, int log2, RegionSave& s) const;
  void restore_region(int x, int y, int log2, const RegionSave& s);
  void mpm_list(int x0, int y0, int list[3]) const;
  int qp_of(int cqp, int c) const;
  // entropy coding
  bool write_slices(ByteBuffer* out, List<H265EncSlice>* slices, bool sao, bool drift_check);
  void write_ctu(CabacWriter& cw, int ctb, bool sao_luma, bool sao_chroma);
  void write_sao(CabacWriter& cw, int ctb, bool sao_luma, bool sao_chroma);
  void write_quadtree(CabacWriter& cw, int x, int y, int log2, int depth);
  void write_cu(CabacWriter& cw, int x, int y, int log2);
  void write_pu(CabacWriter& cw, int x, int y, const CuInfo& cu);
  void write_tu_tree(CabacWriter& cw, int x, int y, int log2, int depth, bool intra, int intra_mode, bool parent_cb, bool parent_cr);
  void write_residual(CabacWriter& cw, int c, int x, int y, int log2, bool intra, int intra_mode);
  bool block_nonzero(int c, int x, int y, int log2) const;
  bool slice_header(BitWriter& w, int slice, bool sao, bool drift_check);
  // reconstruction
  bool decode(ConstByteView stream, ArrayView<const H265EncSlice> slices, bool drift_check);
  void choose_sao();

  allocators::IAllocator& allocator_;
  const H265EncPicture* pic_ = nullptr;
  EncPlanes16 src_;
  int W_ = 0, H_ = 0, cw_ = 0, ch_ = 0, bd_ = 8, qp_bd_ = 0;
  int log2_ctb_ = 5, ctb_w_ = 0, ctb_h_ = 0, w8_ = 0, h8_ = 0;
  int slice_type_ = kSliceI;
  int slice_start_ = 0;
  Array<uint16_t> rec_[3];
  Array<int16_t> coef_[3];
  Array<Cell> cells_;
  Array<int8_t> ctb_qp_;
  Array<EncSao> ctb_sao_;
  Array<uint8_t> luma8_;          // 8-bit lumas for the motion estimator: the current picture, then each reference used
  Array<MotionBlockResult> motion_;
  int motion_blocks_ = 0;
  int motion_slot_[kH265EncMaxReferences] = {};   // per dpb entry: its block vectors in motion_ (-1: none)
  RegionSave save_[2][4];        // [0]: the best candidate of a depth, [1]: scratch
  // the reference picture set and lists
  int st_before_[8], st_after_[8], lt_curr_[8];   // dpb indices of the pictures used by the current picture, in RPS order
  int n_before_ = 0, n_after_ = 0, n_lt_ = 0;
  int neg_[16], pos_[16], lt_[16];                // every dpb entry in RPS order (used or not)
  int n_neg_ = 0, n_pos_ = 0, n_ltall_ = 0;
  RefEntry list_[2][16];
  int num_ref_[2] = {0, 0};
  int list_entry_[2][16];
  bool modify_[2] = {false, false};
  int st_rps_bits_ = 0;
  // slices
  List<uint32_t> slice_bounds_;   // the first coding tree block of every slice, then the end
  uint32_t slice_count_ = 0;
  // the coding state of the CTU being coded
  int ctu_qp_ = 26;
  int qp_prev_ = 26;
  bool delta_coded_ = false;
  int qp_pred_ = 26;
  ByteBuffer nal_;
  ByteBuffer pass_;   // the slices of the SAO-less pass
  List<H265EncSlice> slices_tmp_;
  UniquePointer<Core> core_;
  Picture refpic_[kH265EncMaxReferences];
  Picture target_;
  int64_t sse_scale_ = 1;
  uint64_t drift_ = 0;
  EncStatistics* stats_ = nullptr;
};

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------
// Prediction (the decoder's: H265Predict.cpp, H265Slice.cpp)

// 6.4.2
bool PictureEncoder::available_pb(int x_cb, int y_cb, int n_cb, int x_pb, int y_pb, int xn, int yn) const {
  const bool same_cb = x_cb <= xn && y_cb <= yn && x_cb + n_cb > xn && y_cb + n_cb > yn;
  bool avail = same_cb ? true : available(x_pb, y_pb, xn, yn);
  if (avail && (cell(xn, yn).mode & 3) != kModeInter) avail = false;
  return avail;
}

// 8.4.4.2 for a block of component c at (x0, y0) of that component, into out (n x n).
void PictureEncoder::predict_intra(int x0, int y0, int log2, int c, int mode, int* out) const {
  const int bd = bd_;
  const int max_value = (1 << bd) - 1;
  const int n = 1 << log2;
  const int f = c ? 2 : 1;
  const ArrayView<uint16_t> plane = rec_[c].view();
  const int stride = c ? cw_ : W_;
  int ref[4 * 32 + 1];
  bool avail[4 * 32 + 1];
  const int total = 4 * n + 1;
  const int unit = c ? 2 : 4;
  const int x_cur = x0 * f, y_cur = y0 * f;
  const bool cip = pic_->seq.constrained_intra_pred;
  auto sample_ok = [&](int xn, int yn) {
    if (!available(x_cur, y_cur, xn * f, yn * f)) return false;
    if (cip && (cell(xn * f, yn * f).mode & 3) != kModeIntra) return false;
    return true;
  };
  bool any = false;
  for (int i = 0; i < 2 * n; i += unit) {
    const int yn = y0 + 2 * n - 1 - i;
    const bool ok = sample_ok(x0 - 1, yn);
    for (int k = 0; k < unit && i + k < 2 * n; ++k) {
      avail[i + k] = ok;
      if (ok) ref[i + k] = plane[static_cast<uint64_t>(yn - k) * stride + x0 - 1];
    }
    any = any || ok;
  }
  {
    const bool ok = sample_ok(x0 - 1, y0 - 1);
    avail[2 * n] = ok;
    if (ok) ref[2 * n] = plane[static_cast<uint64_t>(y0 - 1) * stride + x0 - 1];
    any = any || ok;
  }
  for (int i = 0; i < 2 * n; i += unit) {
    const bool ok = sample_ok(x0 + i, y0 - 1);
    for (int k = 0; k < unit && i + k < 2 * n; ++k) {
      avail[2 * n + 1 + i + k] = ok;
      if (ok) ref[2 * n + 1 + i + k] = plane[static_cast<uint64_t>(y0 - 1) * stride + x0 + i + k];
    }
    any = any || ok;
  }
  if (!any) {
    for (int i = 0; i < total; ++i) ref[i] = 1 << (bd - 1);
  } else {
    if (!avail[0]) {
      int j = 1;
      while (j < total && !avail[j]) ++j;
      ref[0] = ref[j];
    }
    for (int i = 1; i < total; ++i)
      if (!avail[i]) ref[i] = ref[i - 1];
  }
  int left[65], top[65];
  left[0] = top[0] = ref[2 * n];
  for (int i = 0; i < 2 * n; ++i) {
    left[1 + i] = ref[2 * n - 1 - i];
    top[1 + i] = ref[2 * n + 1 + i];
  }
  if (c == 0 && mode != 1 && n != 4) {
    const int dist = imin(iabs(mode - 26), iabs(mode - 10));
    const int thres = n == 8 ? 7 : n == 16 ? 1 : 0;
    if (dist > thres) {
      int fl[65], ft[65];
      fl[0] = ft[0] = (left[1] + 2 * left[0] + top[1] + 2) >> 2;
      for (int i = 1; i < 2 * n; ++i) {
        fl[i] = (left[i + 1] + 2 * left[i] + left[i - 1] + 2) >> 2;
        ft[i] = (top[i + 1] + 2 * top[i] + top[i - 1] + 2) >> 2;
      }
      fl[1] = (left[2] + 2 * left[1] + left[0] + 2) >> 2;
      ft[1] = (top[2] + 2 * top[1] + top[0] + 2) >> 2;
      fl[2 * n] = left[2 * n];
      ft[2 * n] = top[2 * n];
      for (int i = 0; i <= 2 * n; ++i) {
        left[i] = fl[i];
        top[i] = ft[i];
      }
    }
  }
  auto L = [&](int y) { return left[y + 1]; };
  auto T = [&](int x) { return top[x + 1]; };
  if (mode == 0) {
    for (int y = 0; y < n; ++y)
      for (int x = 0; x < n; ++x) out[y * n + x] = ((n - 1 - x) * L(y) + (x + 1) * T(n) + (n - 1 - y) * T(x) + (y + 1) * L(n) + n) >> (log2 + 1);
  } else if (mode == 1) {
    int sum = n;
    for (int i = 0; i < n; ++i) sum += T(i) + L(i);
    const int dc = sum >> (log2 + 1);
    for (int i = 0; i < n * n; ++i) out[i] = dc;
    if (c == 0 && n < 32) {
      out[0] = (L(0) + 2 * dc + T(0) + 2) >> 2;
      for (int x = 1; x < n; ++x) out[x] = (T(x) + 3 * dc + 2) >> 2;
      for (int y = 1; y < n; ++y) out[y * n] = (L(y) + 3 * dc + 2) >> 2;
    }
  } else {
    const int angle = kIntraPredAngle[mode];
    int refbuf[3 * 32 + 3];
    int* r = refbuf + 32 + 1;
    if (mode >= 18) {
      for (int x = 0; x <= n; ++x) r[x] = T(x - 1);
      if (angle < 0) {
        const int last = (n * angle) >> 5;
        if (last < -1)
          for (int x = last; x <= -1; ++x) r[x] = L(-1 + ((x * kInvAngle[mode] + 128) >> 8));
      } else {
        for (int x = n + 1; x <= 2 * n; ++x) r[x] = T(x - 1);
      }
      for (int y = 0; y < n; ++y) {
        const int idx = ((y + 1) * angle) >> 5, fact = ((y + 1) * angle) & 31;
        for (int x = 0; x < n; ++x) out[y * n + x] = fact ? ((32 - fact) * r[x + idx + 1] + fact * r[x + idx + 2] + 16) >> 5 : r[x + idx + 1];
      }
      if (mode == 26 && c == 0 && n < 32)
        for (int y = 0; y < n; ++y) out[y * n] = clip3(0, max_value, T(0) + ((L(y) - L(-1)) >> 1));
    } else {
      for (int x = 0; x <= n; ++x) r[x] = L(x - 1);
      if (angle < 0) {
        const int last = (n * angle) >> 5;
        if (last < -1)
          for (int x = last; x <= -1; ++x) r[x] = T(-1 + ((x * kInvAngle[mode] + 128) >> 8));
      } else {
        for (int x = n + 1; x <= 2 * n; ++x) r[x] = L(x - 1);
      }
      for (int x = 0; x < n; ++x) {
        const int idx = ((x + 1) * angle) >> 5, fact = ((x + 1) * angle) & 31;
        for (int y = 0; y < n; ++y) out[y * n + x] = fact ? ((32 - fact) * r[y + idx + 1] + fact * r[y + idx + 2] + 16) >> 5 : r[y + idx + 1];
      }
      if (mode == 10 && c == 0 && n < 32)
        for (int x = 0; x < n; ++x) out[x] = clip3(0, max_value, L(0) + ((T(x) - T(-1)) >> 1));
    }
  }
}

namespace {
// 8.5.3.3.3: the 14-bit intermediate prediction of a w x h block (whole position xi, yi, fraction fx, fy) of a plane of pw x ph samples (borders replicated).
void interpolate(ArrayView<const uint16_t> plane, int pw, int ph, int xi, int yi, int fx, int fy, int w, int h, int bd, bool chroma, int16_t* out) {
  const int taps = chroma ? 4 : 8, before = chroma ? 1 : 3;
  const int shift1 = imin(4, bd - 8), shift3 = imax(2, 14 - bd);
  auto at = [&](int x, int y) -> int { return plane[static_cast<uint64_t>(clip3(0, ph - 1, y)) * pw + clip3(0, pw - 1, x)]; };
  auto coef = [&](int frac, int k) -> int { return chroma ? kChromaFilter[frac][k] : kLumaFilter[frac][k]; };
  if (fx == 0 && fy == 0) {
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) out[y * w + x] = static_cast<int16_t>(at(xi + x, yi + y) << shift3);
  } else if (fy == 0) {
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        int s = 0;
        for (int k = 0; k < taps; ++k) s += coef(fx, k) * at(xi + x - before + k, yi + y);
        out[y * w + x] = static_cast<int16_t>(s >> shift1);
      }
  } else if (fx == 0) {
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        int s = 0;
        for (int k = 0; k < taps; ++k) s += coef(fy, k) * at(xi + x, yi + y - before + k);
        out[y * w + x] = static_cast<int16_t>(s >> shift1);
      }
  } else {
    int16_t tmp[(64 + 7) * 64];
    for (int y = 0; y < h + taps - 1; ++y)
      for (int x = 0; x < w; ++x) {
        int s = 0;
        for (int k = 0; k < taps; ++k) s += coef(fx, k) * at(xi + x - before + k, yi + y - before);
        tmp[y * w + x] = static_cast<int16_t>(s >> shift1);
      }
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        int s = 0;
        for (int k = 0; k < taps; ++k) s += coef(fy, k) * tmp[(y + k) * w + x];
        out[y * w + x] = static_cast<int16_t>(s >> 6);
      }
  }
}
}  // namespace

void PictureEncoder::predict_luma(const RefEntry& r, int x0, int y0, int w, int h, int mvx, int mvy, int* out) const {
  int16_t tmp[64 * 64];
  const H265EncReference& ref = pic_->dpb[static_cast<uint64_t>(r.dpb)];
  interpolate(ref.picture.y, W_, H_, x0 + (mvx >> 2), y0 + (mvy >> 2), mvx & 3, mvy & 3, w, h, bd_, false, tmp);
  const int shift = 14 - bd_, offset = 1 << (shift - 1), max_value = (1 << bd_) - 1;
  for (int i = 0; i < w * h; ++i) out[i] = clip3(0, max_value, (tmp[i] + offset) >> shift);
}

// 8.5.3.3 without weighted prediction, for the three components.
void PictureEncoder::predict_inter(int x0, int y0, int w, int h, const MvField& m, int* out_y, int* out_u, int* out_v) const {
  int16_t pred[2][64 * 64];
  int* outs[3] = {out_y, out_u, out_v};
  for (int c = 0; c < 3; ++c) {
    const int cw = c ? w / 2 : w, chh = c ? h / 2 : h;
    const int cx = c ? x0 / 2 : x0, cy = c ? y0 / 2 : y0;
    const int pw = c ? cw_ : W_, ph = c ? ch_ : H_;
    int used = 0;
    for (int l = 0; l < 2; ++l) {
      if (!(m.pred_flags & (1 << l))) continue;
      const H265EncReference& ref = pic_->dpb[static_cast<uint64_t>(list_[l][m.ref_idx[l]].dpb)];
      const ArrayView<const uint16_t> plane = c == 0 ? ref.picture.y : c == 1 ? ref.picture.u : ref.picture.v;
      if (c == 0)
        interpolate(plane, pw, ph, x0 + (m.mv[l][0] >> 2), y0 + (m.mv[l][1] >> 2), m.mv[l][0] & 3, m.mv[l][1] & 3, w, h, bd_, false, pred[used]);
      else
        interpolate(plane, pw, ph, cx + (m.mv[l][0] >> 3), cy + (m.mv[l][1] >> 3), m.mv[l][0] & 7, m.mv[l][1] & 7, cw, chh, bd_, true, pred[used]);
      ++used;
    }
    const int max_value = (1 << bd_) - 1, shift1 = 14 - bd_;
    int* o = outs[c];
    if (used == 1) {
      const int offset = 1 << (shift1 - 1);
      for (int i = 0; i < cw * chh; ++i) o[i] = clip3(0, max_value, (pred[0][i] + offset) >> shift1);
    } else {
      const int shift2 = shift1 + 1, offset = 1 << (shift2 - 1);
      for (int i = 0; i < cw * chh; ++i) o[i] = clip3(0, max_value, (pred[0][i] + pred[1][i] + offset) >> shift2);
    }
  }
}

// 8.5.3.2.2 - 8.5.3.2.5 (no temporal candidate: sps_temporal_mvp_enabled_flag 0): the five merge candidates of a 2Nx2N prediction unit.
void PictureEncoder::merge_candidates(int x0, int y0, int n, MvField* cands) const {
  int count = 0;
  const int max_cand = 5;
  auto at = [&](int x, int y) -> const MvField& { return cell(x, y).mv; };
  const int w = n, h = n;
  bool el_a1, el_b1, el_b0, el_a0, el_b2;
  MvField a1{}, b1{}, b0{}, a0{}, b2{};
  el_a1 = available_pb(x0, y0, n, x0, y0, x0 - 1, y0 + h - 1);
  if (el_a1) a1 = at(x0 - 1, y0 + h - 1);
  el_b1 = available_pb(x0, y0, n, x0, y0, x0 + w - 1, y0 - 1);
  if (el_b1) b1 = at(x0 + w - 1, y0 - 1);
  el_b0 = available_pb(x0, y0, n, x0, y0, x0 + w, y0 - 1);
  if (el_b0) b0 = at(x0 + w, y0 - 1);
  el_a0 = available_pb(x0, y0, n, x0, y0, x0 - 1, y0 + h);
  if (el_a0) a0 = at(x0 - 1, y0 + h);
  el_b2 = available_pb(x0, y0, n, x0, y0, x0 - 1, y0 - 1);
  if (el_b2) b2 = at(x0 - 1, y0 - 1);
  const bool av_a1 = el_a1;
  const bool av_b1 = el_b1 && !(el_a1 && same_motion(a1, b1));
  const bool av_b0 = el_b0 && !(el_b1 && same_motion(b1, b0));
  const bool av_a0 = el_a0 && !(el_a1 && same_motion(a1, a0));
  const bool av_b2 = el_b2 && !(el_a1 && same_motion(a1, b2)) && !(el_b1 && same_motion(b1, b2)) && !(av_a0 && av_b0 && av_a1 && av_b1);
  if (av_a1) cands[count++] = a1;
  if (av_b1) cands[count++] = b1;
  if (av_b0) cands[count++] = b0;
  if (av_a0) cands[count++] = a0;
  if (av_b2) cands[count++] = b2;
  if (count > max_cand) count = max_cand;
  if (slice_type_ == kSliceB && count > 1 && count < max_cand) {
    static const int kL0[12] = {0, 1, 0, 2, 1, 2, 0, 3, 1, 3, 2, 3};
    static const int kL1[12] = {1, 0, 2, 0, 2, 1, 3, 0, 3, 1, 3, 2};
    const int orig = count;
    for (int comb = 0; comb < 12; ++comb) {
      const MvField c0 = cands[kL0[comb]];
      const MvField c1 = cands[kL1[comb]];
      if ((c0.pred_flags & 1) && (c1.pred_flags & 2) &&
          (list_[0][c0.ref_idx[0]].poc != list_[1][c1.ref_idx[1]].poc || c0.mv[0][0] != c1.mv[1][0] || c0.mv[0][1] != c1.mv[1][1])) {
        MvField m{};
        m.pred_flags = 3;
        m.ref_idx[0] = c0.ref_idx[0];
        m.ref_idx[1] = c1.ref_idx[1];
        m.mv[0][0] = c0.mv[0][0];
        m.mv[0][1] = c0.mv[0][1];
        m.mv[1][0] = c1.mv[1][0];
        m.mv[1][1] = c1.mv[1][1];
        cands[count++] = m;
      }
      if (comb + 1 == orig * (orig - 1) || count == max_cand) break;
    }
  }
  const int num_ref = slice_type_ == kSliceP ? num_ref_[0] : imin(num_ref_[0], num_ref_[1]);
  for (int zero = 0; count < max_cand; ++zero) {
    MvField m{};
    const int ref = zero < num_ref ? zero : 0;
    m.ref_idx[0] = static_cast<int8_t>(ref);
    m.ref_idx[1] = slice_type_ == kSliceB ? static_cast<int8_t>(ref) : static_cast<int8_t>(-1);
    m.pred_flags = slice_type_ == kSliceB ? 3 : 1;
    cands[count++] = m;
  }
  for (int i = 0; i < max_cand; ++i)
    for (int l = 0; l < 2; ++l)
      if (!(cands[i].pred_flags & (1 << l))) {
        cands[i].ref_idx[l] = -1;
        cands[i].mv[l][0] = cands[i].mv[l][1] = 0;
      }
}

// 8.5.3.2.6 / 8.5.3.2.7: both motion vector predictor candidates (mvp_flag 0 and 1).
void PictureEncoder::amvp(int x_pb, int y_pb, int n, int list, int ref_idx, int out[2][2]) const {
  const RefEntry& target = list_[list][ref_idx];
  const int w = n, h = n;
  auto at = [&](int x, int y) -> const MvField& { return cell(x, y).mv; };
  const int xa0 = x_pb - 1, ya0 = y_pb + h, xa1 = x_pb - 1, ya1 = y_pb + h - 1;
  const int xb0 = x_pb + w, yb0 = y_pb - 1, xb1 = x_pb + w - 1, yb1 = y_pb - 1, xb2 = x_pb - 1, yb2 = y_pb - 1;
  const bool av_a0 = available_pb(x_pb, y_pb, n, x_pb, y_pb, xa0, ya0);
  const bool av_a1 = available_pb(x_pb, y_pb, n, x_pb, y_pb, xa1, ya1);
  const bool is_scaled = av_a0 || av_a1;
  auto exact = [&](const MvField& nb, int mv[2]) {
    for (int k = 0; k < 2; ++k) {
      const int l = k == 0 ? list : 1 - list;
      if ((nb.pred_flags & (1 << l)) && list_[l][nb.ref_idx[l]].dpb == target.dpb) {
        mv[0] = nb.mv[l][0];
        mv[1] = nb.mv[l][1];
        return true;
      }
    }
    return false;
  };
  auto scaled = [&](const MvField& nb, int mv[2]) {
    for (int k = 0; k < 2; ++k) {
      const int l = k == 0 ? list : 1 - list;
      if ((nb.pred_flags & (1 << l)) && list_[l][nb.ref_idx[l]].long_term == target.long_term) {
        const int ref_poc_n = list_[l][nb.ref_idx[l]].poc;
        if (!target.long_term && !list_[l][nb.ref_idx[l]].long_term) {
          scale_mv(poc_diff(pic_->poc, ref_poc_n), poc_diff(pic_->poc, target.poc), nb.mv[l], mv);
        } else {
          mv[0] = nb.mv[l][0];
          mv[1] = nb.mv[l][1];
        }
        return true;
      }
    }
    return false;
  };
  int mv_a[2] = {0, 0}, mv_b[2] = {0, 0};
  bool flag_a = false, flag_b = false;
  const int pa_x[2] = {xa0, xa1}, pa_y[2] = {ya0, ya1};
  const bool pa_av[2] = {av_a0, av_a1};
  for (int k = 0; k < 2 && !flag_a; ++k)
    if (pa_av[k]) flag_a = exact(at(pa_x[k], pa_y[k]), mv_a);
  for (int k = 0; k < 2 && !flag_a; ++k)
    if (pa_av[k]) flag_a = scaled(at(pa_x[k], pa_y[k]), mv_a);
  const int pb_x[3] = {xb0, xb1, xb2}, pb_y[3] = {yb0, yb1, yb2};
  bool pb_av[3];
  for (int k = 0; k < 3; ++k) pb_av[k] = available_pb(x_pb, y_pb, n, x_pb, y_pb, pb_x[k], pb_y[k]);
  for (int k = 0; k < 3 && !flag_b; ++k)
    if (pb_av[k]) flag_b = exact(at(pb_x[k], pb_y[k]), mv_b);
  if (!is_scaled && flag_b) {
    mv_a[0] = mv_b[0];
    mv_a[1] = mv_b[1];
    flag_a = true;
  }
  if (!is_scaled) {
    flag_b = false;
    for (int k = 0; k < 3 && !flag_b; ++k)
      if (pb_av[k]) flag_b = scaled(at(pb_x[k], pb_y[k]), mv_b);
  }
  int cnt = 0;
  if (flag_a) {
    out[cnt][0] = mv_a[0];
    out[cnt][1] = mv_a[1];
    ++cnt;
  }
  if (flag_b && !(flag_a && mv_a[0] == mv_b[0] && mv_a[1] == mv_b[1])) {
    out[cnt][0] = mv_b[0];
    out[cnt][1] = mv_b[1];
    ++cnt;
  }
  while (cnt < 2) {
    out[cnt][0] = out[cnt][1] = 0;
    ++cnt;
  }
}

// 8.4.2: the most probable modes of the luma block at (x0, y0).
void PictureEncoder::mpm_list(int x0, int y0, int list[3]) const {
  int cand_a = 1, cand_b = 1;
  if (available(x0, y0, x0 - 1, y0)) {
    const Cell& k = cell(x0 - 1, y0);
    if ((k.mode & 3) == kModeIntra) cand_a = k.intra_mode;
  }
  if (available(x0, y0, x0, y0 - 1) && y0 - 1 >= ((y0 >> log2_ctb_) << log2_ctb_)) {
    const Cell& k = cell(x0, y0 - 1);
    if ((k.mode & 3) == kModeIntra) cand_b = k.intra_mode;
  }
  if (cand_a == cand_b) {
    if (cand_a < 2) {
      list[0] = 0;
      list[1] = 1;
      list[2] = 26;
    } else {
      list[0] = cand_a;
      list[1] = 2 + ((cand_a - 2 - 1 + 32) & 31);
      list[2] = 2 + ((cand_a - 2 + 1) & 31);
    }
  } else {
    list[0] = cand_a;
    list[1] = cand_b;
    if (cand_a != 0 && cand_b != 0) list[2] = 0;
    else if (cand_a != 1 && cand_b != 1) list[2] = 1;
    else list[2] = 26;
  }
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------
// Residuals: forward transform and quantization, then the decoder's scaling and inverse transform (H265Predict.cpp reconstruct_px)

// Qp'Y or Qp'C of a coding unit of QpY cqp (8.6.1).
int PictureEncoder::qp_of(int cqp, int c) const {
  if (c == 0) return cqp + qp_bd_;
  static const int kQpC[14] = {29, 30, 31, 32, 33, 33, 34, 34, 35, 35, 36, 36, 37, 37};
  const int qpi = clip3(-qp_bd_, 57, cqp);
  const int qpc = qpi < 30 ? qpi : qpi >= 43 ? qpi - 6 : kQpC[qpi - 30];
  return qpc + qp_bd_;
}

// Codes the n x n block of component c at (x, y) of that component against its prediction: the levels into coef_, the reconstruction into rec_. Returns the squared error
// (scaled to 8 bits' range x 256 by the caller), *bits an estimate of the levels' bits.
int64_t PictureEncoder::code_block(int c, int x, int y, int log2, const int* pred, int qp, bool intra, int* bits, bool* nonzero) {
  const int n = 1 << log2;
  const int stride = c ? cw_ : W_;
  const ArrayView<const uint16_t> src = c == 0 ? src_.y : c == 1 ? src_.u : src_.v;
  const ArrayView<uint16_t> rec = rec_[c].view();
  const ArrayView<int16_t> coefs = coef_[c].view();
  const int step = 32 / n;
  int32_t resid[32 * 32], tmp[32 * 32], level[32 * 32];
  for (int j = 0; j < n; ++j)
    for (int i = 0; i < n; ++i) resid[j * n + i] = src[static_cast<uint64_t>(y + j) * stride + x + i] - pred[j * n + i];
  // forward: columns, then rows (HM's shifts: log2 + bitDepth - 9, then log2 + 6)
  const int s1 = log2 + bd_ - 9, s2 = log2 + 6;
  for (int k = 0; k < n; ++k)
    for (int i = 0; i < n; ++i) {
      int64_t e = 0;
      for (int j = 0; j < n; ++j) e += static_cast<int64_t>(kTransform[k * step][j]) * resid[j * n + i];
      tmp[k * n + i] = static_cast<int32_t>((e + (int64_t{1} << (s1 - 1))) >> s1);
    }
  const int qbits = 14 + qp / 6 + (15 - bd_ - log2);
  const int64_t q = kQuantScale[qp % 6];
  const int64_t round = (intra ? int64_t{171} : int64_t{85}) << (qbits - 9);
  bool any = false;
  int b = 0;
  for (int k = 0; k < n; ++k)
    for (int l = 0; l < n; ++l) {
      int64_t e = 0;
      for (int i = 0; i < n; ++i) e += static_cast<int64_t>(kTransform[l * step][i]) * tmp[k * n + i];
      const int64_t v = (e + (int64_t{1} << (s2 - 1))) >> s2;
      const int64_t a = ((v < 0 ? -v : v) * q + round) >> qbits;
      const int32_t lv = static_cast<int32_t>(a > 32767 ? 32767 : a);
      level[k * n + l] = v < 0 ? -lv : lv;
      if (lv) {
        any = true;
        b += lv == 1 ? 3 : 5 + 2 * ilog2(static_cast<uint32_t>(lv));
      }
    }
  *nonzero = any;
  *bits = any ? b + 6 : 1;
  for (int j = 0; j < n; ++j)
    for (int i = 0; i < n; ++i) coefs[static_cast<uint64_t>(y + j) * stride + x + i] = static_cast<int16_t>(level[j * n + i]);
  const int max_value = (1 << bd_) - 1;
  int64_t sse = 0;
  if (!any) {
    for (int j = 0; j < n; ++j)
      for (int i = 0; i < n; ++i) {
        const int v = pred[j * n + i];
        rec[static_cast<uint64_t>(y + j) * stride + x + i] = static_cast<uint16_t>(v);
        const int d = v - src[static_cast<uint64_t>(y + j) * stride + x + i];
        sse += d * d;
      }
    return sse;
  }
  // 8.6.2 / 8.6.4.2 exactly as the decoder
  const int bd_shift = bd_ + log2 - 5;
  const int64_t scale = static_cast<int64_t>(kLevelScale[qp % 6]) << (qp / 6);
  int32_t d[32 * 32], g[32 * 32];
  for (int i = 0; i < n * n; ++i) {
    if (level[i] == 0) {
      d[i] = 0;
      continue;
    }
    const int64_t v = (static_cast<int64_t>(level[i]) * 16 * scale + (int64_t{1} << (bd_shift - 1))) >> bd_shift;
    d[i] = static_cast<int32_t>(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
  }
  const int final_shift = 20 - bd_;
  const int64_t final_round = int64_t{1} << (final_shift - 1);
  for (int xx = 0; xx < n; ++xx)
    for (int yy = 0; yy < n; ++yy) {
      int64_t e = 0;
      for (int j = 0; j < n; ++j) {
        const int cj = d[j * n + xx];
        if (cj) e += static_cast<int64_t>(kTransform[j * step][yy]) * cj;
      }
      const int64_t t = (e + 64) >> 7;
      g[yy * n + xx] = static_cast<int32_t>(t < -32768 ? -32768 : t > 32767 ? 32767 : t);
    }
  for (int yy = 0; yy < n; ++yy)
    for (int xx = 0; xx < n; ++xx) {
      int64_t e = 0;
      for (int j = 0; j < n; ++j) e += static_cast<int64_t>(kTransform[j * step][xx]) * g[yy * n + j];
      const int r = static_cast<int32_t>((e + final_round) >> final_shift);
      const int v = clip3(0, max_value, pred[yy * n + xx] + r);
      rec[static_cast<uint64_t>(y + yy) * stride + x + xx] = static_cast<uint16_t>(v);
      const int dd = v - src[static_cast<uint64_t>(y + yy) * stride + x + xx];
      sse += dd * dd;
    }
  return sse;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------
// Decisions

void PictureEncoder::save_region(int x, int y, int log2, RegionSave& s) const {
  const int n = 1 << log2;
  for (int c = 0; c < 3; ++c) {
    const int cn = c ? n / 2 : n, cx = c ? x / 2 : x, cy = c ? y / 2 : y, stride = c ? cw_ : W_;
    for (int j = 0; j < cn; ++j)
      for (int i = 0; i < cn; ++i) {
        const uint64_t at = static_cast<uint64_t>(cy + j) * stride + cx + i;
        s.rec[c][j * cn + i] = rec_[c][at];
        s.coef[c][j * cn + i] = coef_[c][at];
      }
  }
  const int cells = n >> 3;
  for (int j = 0; j < cells; ++j)
    for (int i = 0; i < cells; ++i) s.cells[j * cells + i] = cells_[static_cast<uint64_t>((y >> 3) + j) * w8_ + (x >> 3) + i];
}
void PictureEncoder::restore_region(int x, int y, int log2, const RegionSave& s) {
  const int n = 1 << log2;
  for (int c = 0; c < 3; ++c) {
    const int cn = c ? n / 2 : n, cx = c ? x / 2 : x, cy = c ? y / 2 : y, stride = c ? cw_ : W_;
    for (int j = 0; j < cn; ++j)
      for (int i = 0; i < cn; ++i) {
        const uint64_t at = static_cast<uint64_t>(cy + j) * stride + cx + i;
        rec_[c][at] = s.rec[c][j * cn + i];
        coef_[c][at] = s.coef[c][j * cn + i];
      }
  }
  const int cells = n >> 3;
  for (int j = 0; j < cells; ++j)
    for (int i = 0; i < cells; ++i) cells_[static_cast<uint64_t>((y >> 3) + j) * w8_ + (x >> 3) + i] = s.cells[j * cells + i];
}

void PictureEncoder::set_cu_cells(int x, int y, int log2, int depth, uint8_t mode, int intra_mode, const MvField& m, const CuInfo& info) {
  const int cells = 1 << (log2 - 3);
  for (int j = 0; j < cells; ++j)
    for (int i = 0; i < cells; ++i) {
      Cell& k = cells_[static_cast<uint64_t>((y >> 3) + j) * w8_ + (x >> 3) + i];
      k.depth = static_cast<uint8_t>(depth);
      k.log2 = static_cast<uint8_t>(log2);
      k.mode = mode;
      k.intra_mode = static_cast<uint8_t>(intra_mode);
      k.mv = m;
      if (i == 0 && j == 0) k.cu = info;
    }
}

int64_t PictureEncoder::eval_intra(int x, int y, int log2, int depth, int mode, int mode_bits) {
  CuInfo info;
  info.intra = true;
  info.intra_mode = static_cast<uint8_t>(mode);
  MvField none{};
  none.ref_idx[0] = none.ref_idx[1] = -1;
  set_cu_cells(x, y, log2, depth, kModeIntra, mode, none, info);
  const int t = imin(log2, pic_->seq.log2_max_tb), tn = 1 << t;
  const int n = 1 << log2;
  int pred[32 * 32];
  int64_t sse = 0;
  int bits = mode_bits + 1;
  const int qy = qp_of(ctu_qp_, 0), qc = qp_of(ctu_qp_, 1);
  // the transform blocks in z-order (the coding block is split into equal ones)
  const int count = (n / tn) * (n / tn);
  for (int k = 0; k < count; ++k) {
    int bx = 0, by = 0;
    for (int b = 0; (1 << (2 * b)) < count; ++b) {
      bx |= ((k >> (2 * b)) & 1) << b;
      by |= ((k >> (2 * b + 1)) & 1) << b;
    }
    const int tx = x + bx * tn, ty = y + by * tn;
    int bb;
    bool nz;
    predict_intra(tx, ty, t, 0, mode, pred);
    sse += code_block(0, tx, ty, t, pred, qy, true, &bb, &nz);
    bits += bb;
    for (int c = 1; c < 3; ++c) {
      predict_intra(tx / 2, ty / 2, t - 1, c, mode, pred);
      sse += code_block(c, tx / 2, ty / 2, t - 1, pred, qc, true, &bb, &nz);
      bits += bb;
    }
  }
  return sse * sse_scale_ + int64_t{kLambda[imin(63, ctu_qp_ + 12)]} * bits;
}

int64_t PictureEncoder::eval_inter(int x, int y, int log2, int depth, const CuInfo& info_in, const MvField& m, int syntax_bits, bool residual) {
  CuInfo info = info_in;
  const int n = 1 << log2;
  int py[64 * 64], pu[32 * 32], pv[32 * 32];
  predict_inter(x, y, n, n, m, py, pu, pv);
  const int t = imin(log2, pic_->seq.log2_max_tb), tn = 1 << t;
  const int qy = qp_of(ctu_qp_, 0), qc = qp_of(ctu_qp_, 1);
  int64_t sse = 0;
  int bits = syntax_bits;
  bool any = false;
  set_cu_cells(x, y, log2, depth, kModeInter, 1, m, info);
  const int count = (n / tn) * (n / tn);
  int blk[32 * 32];
  for (int k = 0; k < count; ++k) {
    int bx = 0, by = 0;
    for (int b = 0; (1 << (2 * b)) < count; ++b) {
      bx |= ((k >> (2 * b)) & 1) << b;
      by |= ((k >> (2 * b + 1)) & 1) << b;
    }
    const int tx = bx * tn, ty = by * tn;
    for (int c = 0; c < 3; ++c) {
      const int cn = c ? tn / 2 : tn, ox = c ? tx / 2 : tx, oy = c ? ty / 2 : ty, ps = c ? n / 2 : n;
      const int* p = c == 0 ? py : c == 1 ? pu : pv;
      for (int j = 0; j < cn; ++j)
        for (int i = 0; i < cn; ++i) blk[j * cn + i] = p[(oy + j) * ps + ox + i];
      const int cx = (c ? x / 2 : x) + ox, cy = (c ? y / 2 : y) + oy;
      if (residual) {
        int bb;
        bool nz;
        sse += code_block(c, cx, cy, c ? t - 1 : t, blk, c ? qc : qy, false, &bb, &nz);
        bits += bb;
        any = any || nz;
      } else {
        const int stride = c ? cw_ : W_;
        const ArrayView<const uint16_t> src = c == 0 ? src_.y : c == 1 ? src_.u : src_.v;
        for (int j = 0; j < cn; ++j)
          for (int i = 0; i < cn; ++i) {
            const uint64_t at = static_cast<uint64_t>(cy + j) * stride + cx + i;
            rec_[c][at] = static_cast<uint16_t>(blk[j * cn + i]);
            coef_[c][at] = 0;
            const int d = blk[j * cn + i] - src[at];
            sse += d * d;
          }
      }
    }
  }
  if (!any) {
    // no residual: a merge coding unit is then a skipped one (rqt_root_cbf is not coded for merge 2Nx2N), another has rqt_root_cbf 0
    if (info.merge) {
      info.skip = true;
      bits = syntax_bits;
    } else {
      bits = syntax_bits + 1;
    }
  } else {
    bits += info.merge ? 1 : 2;
  }
  set_cu_cells(x, y, log2, depth, static_cast<uint8_t>(kModeInter | (info.skip ? kModeSkip : 0)), 1, m, info);
  return sse * sse_scale_ + int64_t{kLambda[imin(63, ctu_qp_ + 12)]} * bits;
}

namespace {
int mvd_bits(int d) { return d == 0 ? 1 : 3 + 2 * ilog2(static_cast<uint32_t>(iabs(d))); }
}

// Motion search of a 2Nx2N prediction unit in list[list][ref_idx]: the vector with the least SAD + lambda * bits (quarter samples, limited by the precision).
int64_t PictureEncoder::search(int x0, int y0, int n, int list, int ref_idx, const int pred[2][2], int mv[2]) {
  const RefEntry& r = list_[list][ref_idx];
  const int sq = kSqrtLambda[imin(63, ctu_qp_ + 12)] << (bd_ - 8);
  int p[64 * 64];
  auto cost = [&](int mx, int my) -> int64_t {
    predict_luma(r, x0, y0, n, n, mx, my, p);
    int64_t sad = 0;
    for (int j = 0; j < n; ++j)
      for (int i = 0; i < n; ++i) sad += iabs(p[j * n + i] - src_.y[static_cast<uint64_t>(y0 + j) * W_ + x0 + i]);
    const int b0 = mvd_bits(mx - pred[0][0]) + mvd_bits(my - pred[0][1]), b1 = mvd_bits(mx - pred[1][0]) + mvd_bits(my - pred[1][1]);
    return sad * 16 + int64_t{sq} * (b0 < b1 ? b0 : b1);
  };
  const int limit = 4 * 1024;
  auto clampv = [&](int v) { return clip3(-limit, limit, v); };
  int cands[12][2];
  int nc = 0;
  cands[nc][0] = 0; cands[nc][1] = 0; ++nc;
  for (int k = 0; k < 2; ++k) {
    cands[nc][0] = pred[k][0];
    cands[nc][1] = pred[k][1];
    ++nc;
  }
  const int slot = motion_slot_[r.dpb];
  if (slot >= 0) {
    const int bw = (W_ + 15) / 16;
    const int pts[3][2] = {{x0 + n / 2, y0 + n / 2}, {x0, y0}, {x0 + n - 1, y0 + n - 1}};
    for (const auto& pt : pts) {
      const MotionBlockResult& m = motion_[static_cast<uint64_t>(slot) * motion_blocks_ + static_cast<uint64_t>(pt[1] / 16) * bw + pt[0] / 16];
      cands[nc][0] = m.vector.x;
      cands[nc][1] = m.vector.y;
      ++nc;
    }
  }
  int best[2] = {0, 0};
  int64_t best_cost = kInfinite;
  for (int i = 0; i < nc; ++i) {
    const int mx = clampv(cands[i][0] & ~3), my = clampv(cands[i][1] & ~3);
    const int64_t cst = cost(mx, my);
    if (cst < best_cost) {
      best_cost = cst;
      best[0] = mx;
      best[1] = my;
    }
  }
  // whole samples around the best
  const int cx = best[0], cy = best[1];
  for (int dy = -2; dy <= 2; ++dy)
    for (int dx = -2; dx <= 2; ++dx) {
      if (!dx && !dy) continue;
      const int mx = clampv(cx + 4 * dx), my = clampv(cy + 4 * dy);
      const int64_t cst = cost(mx, my);
      if (cst < best_cost) {
        best_cost = cst;
        best[0] = mx;
        best[1] = my;
      }
    }
  const H265EncPrecision prec = pic_->precision;
  for (int stage = 0; stage < 2; ++stage) {
    if (prec == H265EncPrecision::Full || (stage == 1 && prec == H265EncPrecision::Half)) break;
    const int d = stage == 0 ? 2 : 1;
    const int bx = best[0], by = best[1];
    for (int dy = -1; dy <= 1; ++dy)
      for (int dx = -1; dx <= 1; ++dx) {
        if (!dx && !dy) continue;
        const int mx = clampv(bx + d * dx), my = clampv(by + d * dy);
        const int64_t cst = cost(mx, my);
        if (cst < best_cost) {
          best_cost = cst;
          best[0] = mx;
          best[1] = my;
        }
      }
  }
  mv[0] = best[0];
  mv[1] = best[1];
  return best_cost;
}

int64_t PictureEncoder::decide_cu(int x, int y, int log2, int depth) {
  const int n = 1 << log2;
  const bool inside = x + n <= W_ && y + n <= H_;
  const bool can_split = log2 > pic_->seq.log2_min_cb;
  const int64_t lambda = kLambda[imin(63, ctu_qp_ + 12)];
  int64_t best = kInfinite;
  RegionSave& keep = save_[0][depth];
  if (inside) {
    bool region_is_best = false;
    auto consider = [&](int64_t cost) {
      if (cost < best) {
        best = cost;
        save_region(x, y, log2, keep);
        region_is_best = true;
      } else {
        region_is_best = false;
      }
    };
    // intra: the modes ranked by the SAD of the prediction of the first transform block, then the best three (and the first most probable mode) coded
    {
      int mpm[3];
      mpm_list(x, y, mpm);
      // (the cells of the coding unit must say intra for the reference availability under constrained intra prediction; only the first transform block is predicted)
      CuInfo info;
      info.intra = true;
      MvField none{};
      none.ref_idx[0] = none.ref_idx[1] = -1;
      set_cu_cells(x, y, log2, depth, kModeIntra, 1, none, info);
      const int t = imin(log2, pic_->seq.log2_max_tb), tn = 1 << t;
      int pred[32 * 32];
      int64_t score[35];
      const int sq = kSqrtLambda[imin(63, ctu_qp_ + 12)] << (bd_ - 8);
      for (int mode = 0; mode < 35; ++mode) {
        predict_intra(x, y, t, 0, mode, pred);
        int64_t sad = 0;
        for (int j = 0; j < tn; ++j)
          for (int i = 0; i < tn; ++i) sad += iabs(pred[j * tn + i] - src_.y[static_cast<uint64_t>(y + j) * W_ + x + i]);
        const int mb = (mode == mpm[0]) ? 1 : (mode == mpm[1] || mode == mpm[2]) ? 2 : 6;
        score[mode] = sad * 16 + int64_t{sq} * mb;
      }
      int chosen[4] = {mpm[0], -1, -1, -1};
      for (int k = 1; k < 4; ++k) {
        int64_t bs = kInfinite;
        for (int mode = 0; mode < 35; ++mode) {
          bool used = false;
          for (int q = 0; q < k; ++q) used = used || chosen[q] == mode;
          if (!used && score[mode] < bs) {
            bs = score[mode];
            chosen[k] = mode;
          }
        }
      }
      for (int k = 0; k < 4; ++k) {
        const int mode = chosen[k];
        if (mode < 0) continue;
        const int mb = (mode == mpm[0]) ? 2 : (mode == mpm[1] || mode == mpm[2]) ? 3 : 6;
        consider(eval_intra(x, y, log2, depth, mode, mb + 2));
      }
    }
    if (slice_type_ != kSliceI) {
      // merge (and skip)
      MvField cands[5];
      merge_candidates(x, y, n, cands);
      for (int i = 0; i < 5; ++i) {
        CuInfo info;
        info.merge = true;
        info.merge_idx = static_cast<uint8_t>(i);
        const int sb = 2 + i;
        consider(eval_inter(x, y, log2, depth, info, cands[i], sb, true));
        info.skip = true;
        consider(eval_inter(x, y, log2, depth, info, cands[i], sb - 1, false));
      }
      // motion search per list and reference
      int best_mv[2][2] = {}, best_ref[2] = {-1, -1}, best_mvp[2] = {0, 0};
      int64_t best_cost[2] = {kInfinite, kInfinite};
      int preds_of[2][2][2] = {};
      const int lists = slice_type_ == kSliceB ? 2 : 1;
      for (int l = 0; l < lists; ++l)
        for (int r = 0; r < num_ref_[l]; ++r) {
          int pr[2][2], mv[2];
          amvp(x, y, n, l, r, pr);
          int64_t cst = search(x, y, n, l, r, pr, mv);
          cst += int64_t{kSqrtLambda[imin(63, ctu_qp_ + 12)] << (bd_ - 8)} * (r + 1);
          if (cst < best_cost[l]) {
            best_cost[l] = cst;
            best_ref[l] = r;
            best_mv[l][0] = mv[0];
            best_mv[l][1] = mv[1];
            const int b0 = mvd_bits(mv[0] - pr[0][0]) + mvd_bits(mv[1] - pr[0][1]), b1 = mvd_bits(mv[0] - pr[1][0]) + mvd_bits(mv[1] - pr[1][1]);
            best_mvp[l] = b1 < b0 ? 1 : 0;
            for (int k = 0; k < 2; ++k)
              for (int q = 0; q < 2; ++q) preds_of[l][k][q] = pr[k][q];
          }
        }
      for (int choice = 0; choice < (lists == 2 ? 3 : 1); ++choice) {
        const int pred_idc = choice;
        CuInfo info;
        info.pred_idc = static_cast<uint8_t>(pred_idc);
        MvField m{};
        m.ref_idx[0] = m.ref_idx[1] = -1;
        int bits = 2;
        bool ok = true;
        for (int l = 0; l < 2; ++l) {
          if (!(l == 0 ? pred_idc != kPredL1 : pred_idc != kPredL0)) continue;
          if (best_ref[l] < 0) {
            ok = false;
            continue;
          }
          m.pred_flags = static_cast<uint8_t>(m.pred_flags | (1 << l));
          m.ref_idx[l] = static_cast<int8_t>(best_ref[l]);
          m.mv[l][0] = static_cast<int16_t>(best_mv[l][0]);
          m.mv[l][1] = static_cast<int16_t>(best_mv[l][1]);
          info.ref_idx[l] = static_cast<int8_t>(best_ref[l]);
          info.mvp[l] = static_cast<uint8_t>(best_mvp[l]);
          info.mvd[l][0] = static_cast<int16_t>(best_mv[l][0] - preds_of[l][best_mvp[l]][0]);
          info.mvd[l][1] = static_cast<int16_t>(best_mv[l][1] - preds_of[l][best_mvp[l]][1]);
          bits += mvd_bits(info.mvd[l][0]) + mvd_bits(info.mvd[l][1]) + 1 + best_ref[l];
        }
        if (!ok) continue;
        consider(eval_inter(x, y, log2, depth, info, m, bits, true));
      }
    }
    if (!region_is_best) restore_region(x, y, log2, keep);
    if (can_split) best += lambda;
  }
  if (can_split) {
    const int half = n >> 1;
    int64_t split_cost = inside ? lambda : 0;
    const int ox[4] = {0, half, 0, half}, oy[4] = {0, 0, half, half};
    for (int k = 0; k < 4 && split_cost < best; ++k) {
      const int cx = x + ox[k], cy = y + oy[k];
      if (cx >= W_ || cy >= H_) continue;
      split_cost += decide_cu(cx, cy, log2 - 1, depth + 1);
    }
    if (inside && best <= split_cost) {
      restore_region(x, y, log2, keep);
      return best;
    }
    return split_cost;
  }
  return best;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------
// Entropy coding (the mirror of H265Slice.cpp)

bool PictureEncoder::block_nonzero(int c, int x, int y, int log2) const {
  const int n = 1 << log2, stride = c ? cw_ : W_;
  for (int j = 0; j < n; ++j)
    for (int i = 0; i < n; ++i)
      if (coef_[c][static_cast<uint64_t>(y + j) * stride + x + i]) return true;
  return false;
}

void PictureEncoder::write_residual(CabacWriter& cw, int c, int x0, int y0, int log2, bool intra, int intra_mode) {
  const int stride = c ? cw_ : W_;
  auto coef_at = [&](int xc, int yc) -> int { return coef_[c][static_cast<uint64_t>(y0 + yc) * stride + x0 + xc]; };
  if (pic_->seq.transform_skip && log2 <= 2) cw.decision(kCtxTransformSkip + (c ? 1 : 0), 0);
  int scan_idx = 0;
  if (intra && (log2 == 2 || (log2 == 3 && c == 0))) {
    if (intra_mode >= 6 && intra_mode <= 14) scan_idx = 2;
    else if (intra_mode >= 22 && intra_mode <= 30) scan_idx = 1;
  }
  const uint8_t* sbx = kScan.x[scan_idx][log2 - 2];
  const uint8_t* sby = kScan.y[scan_idx][log2 - 2];
  const uint8_t* px4 = kScan.x[scan_idx][2];
  const uint8_t* py4 = kScan.y[scan_idx][2];
  const int sb_count = 1 << ((log2 - 2) * 2);
  int last_sub = -1, last_pos = 0;
  for (int i = sb_count - 1; i >= 0 && last_sub < 0; --i)
    for (int p = 15; p >= 0; --p)
      if (coef_at((sbx[i] << 2) + px4[p], (sby[i] << 2) + py4[p])) {
        last_sub = i;
        last_pos = p;
        break;
      }
  if (last_sub < 0) return;   // (a block with cbf 1 has a coefficient)
  int last_x = (sbx[last_sub] << 2) + px4[last_pos], last_y = (sby[last_sub] << 2) + py4[last_pos];
  if (scan_idx == 2) {
    const int t = last_x;
    last_x = last_y;
    last_y = t;
  }
  int ctx_offset, ctx_shift;
  if (c == 0) {
    ctx_offset = 3 * (log2 - 2) + ((log2 - 1) >> 2);
    ctx_shift = (log2 + 1) >> 2;
  } else {
    ctx_offset = 15;
    ctx_shift = log2 - 2;
  }
  const int max_prefix = (log2 << 1) - 1;
  auto prefix_of = [](int v) {
    static const uint8_t kGroup[32] = {0, 1, 2, 3, 4, 4, 5, 5, 6, 6, 6, 6, 7, 7, 7, 7, 8, 8, 8, 8, 8, 8, 8, 8, 9, 9, 9, 9, 9, 9, 9, 9};
    return static_cast<int>(kGroup[v]);
  };
  const int pxv = prefix_of(last_x), pyv = prefix_of(last_y);
  for (int i = 0; i < pxv; ++i) cw.decision(kCtxLastX + ctx_offset + (i >> ctx_shift), 1);
  if (pxv < max_prefix) cw.decision(kCtxLastX + ctx_offset + (pxv >> ctx_shift), 0);
  for (int i = 0; i < pyv; ++i) cw.decision(kCtxLastY + ctx_offset + (i >> ctx_shift), 1);
  if (pyv < max_prefix) cw.decision(kCtxLastY + ctx_offset + (pyv >> ctx_shift), 0);
  if (pxv > 3) {
    const int nb = (pxv >> 1) - 1;
    cw.bypass_bits(static_cast<uint32_t>(last_x - (1 << nb) * (2 + (pxv & 1))), nb);
  }
  if (pyv > 3) {
    const int nb = (pyv >> 1) - 1;
    cw.bypass_bits(static_cast<uint32_t>(last_y - (1 << nb) * (2 + (pyv & 1))), nb);
  }
  uint8_t csbf[8][8] = {};
  const int sb_w = 1 << (log2 - 2);
  int c1 = 1;
  for (int i = last_sub; i >= 0; --i) {
    const int xs = sbx[i], ys = sby[i];
    int lv[16];
    bool any = false;
    for (int p = 0; p < 16; ++p) {
      lv[p] = coef_at((xs << 2) + px4[p], (ys << 2) + py4[p]);
      any = any || lv[p] != 0;
    }
    bool infer_dc = false, coded = true;
    int csbf_ctx = 0;
    if (xs < sb_w - 1) csbf_ctx |= csbf[ys][xs + 1];
    if (ys < sb_w - 1) csbf_ctx |= csbf[ys + 1][xs];
    if (i < last_sub && i > 0) {
      coded = any;
      cw.decision(kCtxCodedSubBlock + (c ? 2 : 0) + csbf_ctx, coded ? 1 : 0);
      infer_dc = true;
    }
    csbf[ys][xs] = coded ? 1 : 0;
    if (!coded) continue;
    const int prev_csbf = (xs < sb_w - 1 ? csbf[ys][xs + 1] : 0) | ((ys < sb_w - 1 ? csbf[ys + 1][xs] : 0) << 1);
    uint8_t sig[16] = {};
    int num_sig = 0;
    const int start = i == last_sub ? last_pos - 1 : 15;
    if (i == last_sub) {
      sig[last_pos] = 1;
      ++num_sig;
    }
    for (int p = start; p >= 0; --p) {
      const int xp = px4[p], yp = py4[p];
      if (p > 0 || !infer_dc) {
        int sig_ctx;
        const int xc = (xs << 2) + xp, yc = (ys << 2) + yp;
        if (log2 == 2) {
          sig_ctx = kCtxIdxMap4x4[(yc << 2) + xc];
        } else if (xc + yc == 0) {
          sig_ctx = 0;
        } else {
          if (prev_csbf == 0) sig_ctx = (xp + yp == 0) ? 2 : (xp + yp < 3) ? 1 : 0;
          else if (prev_csbf == 1) sig_ctx = yp == 0 ? 2 : yp == 1 ? 1 : 0;
          else if (prev_csbf == 2) sig_ctx = xp == 0 ? 2 : xp == 1 ? 1 : 0;
          else sig_ctx = 2;
          if (c == 0) {
            if (xs > 0 || ys > 0) sig_ctx += 3;
            sig_ctx += log2 == 3 ? (scan_idx == 0 ? 9 : 15) : 21;
          } else {
            sig_ctx += log2 == 3 ? 9 : 12;
          }
        }
        if (c > 0) sig_ctx += 27;
        const int s = lv[p] != 0;
        cw.decision(kCtxSigCoeff + sig_ctx, s);
        sig[p] = static_cast<uint8_t>(s);
        if (s) {
          ++num_sig;
          infer_dc = false;
        }
      } else {
        sig[0] = 1;   // (inferred; the coded sub-block's only coefficient is the DC one)
        ++num_sig;
      }
    }
    if (num_sig == 0) continue;
    uint8_t greater1[16] = {}, greater2[16] = {};
    int num_g1 = 0, last_g1_pos = -1;
    int ctx_set = (i == 0 || c > 0) ? 0 : 2;
    if (c1 == 0) ++ctx_set;
    c1 = 1;
    for (int p = 15; p >= 0; --p) {
      if (!sig[p]) continue;
      if (num_g1 < 8) {
        const int ctx = ctx_set * 4 + c1 + (c > 0 ? 16 : 0);
        const int g = iabs(lv[p]) > 1;
        cw.decision(kCtxGreater1 + ctx, g);
        greater1[p] = static_cast<uint8_t>(g);
        ++num_g1;
        if (g) {
          c1 = 0;
          if (last_g1_pos == -1) last_g1_pos = p;
        } else if (c1 > 0 && c1 < 3) {
          ++c1;
        }
      }
    }
    if (last_g1_pos != -1) {
      const int g = iabs(lv[last_g1_pos]) > 2;
      cw.decision(kCtxGreater2 + ctx_set + (c > 0 ? 4 : 0), g);
      greater2[last_g1_pos] = static_cast<uint8_t>(g);
    }
    for (int p = 15; p >= 0; --p)
      if (sig[p]) cw.bypass(lv[p] < 0 ? 1 : 0);
    int num_sig_coeff = 0, rice = 0;
    for (int p = 15; p >= 0; --p) {
      if (!sig[p]) continue;
      const int base = 1 + greater1[p] + greater2[p];
      const int level = iabs(lv[p]);
      if (base == ((num_sig_coeff < 8) ? ((p == last_g1_pos) ? 3 : 2) : 1)) {
        // coeff_abs_level_remaining (9.3.3.11)
        const uint32_t v = static_cast<uint32_t>(level - base);
        if ((v >> rice) <= 3) {
          const uint32_t prefix = v >> rice;
          for (uint32_t k = 0; k < prefix; ++k) cw.bypass(1);
          cw.bypass(0);
          cw.bypass_bits(v & ((1u << rice) - 1), rice);
        } else {
          int prefix = 4;
          while ((((1u << (prefix + 1 - 3)) + 2) << rice) <= v) ++prefix;
          for (int k = 0; k < prefix; ++k) cw.bypass(1);
          cw.bypass(0);
          cw.bypass_bits(v - (((1u << (prefix - 3)) + 2) << rice), prefix - 3 + rice);
        }
        if (level > 3 * (1 << rice)) rice = imin(rice + 1, 4);
      }
      ++num_sig_coeff;
    }
  }
}

void PictureEncoder::write_tu_tree(CabacWriter& cw, int x0, int y0, int log2, int depth, bool intra, int intra_mode, bool parent_cb, bool parent_cr) {
  const H265EncSequence& s = pic_->seq;
  const int max_depth = intra ? s.max_th_depth_intra : s.max_th_depth_inter;
  const bool split = log2 > s.log2_max_tb;
  if (log2 <= s.log2_max_tb && log2 > s.log2_min_tb && depth < max_depth) cw.decision(kCtxSplitTransform + 5 - log2, 0);
  // the chroma flags of a node: any coefficient in its chroma (log2 > 2 always here: luma blocks are at least 8x8)
  const bool cb = block_nonzero(1, x0 / 2, y0 / 2, log2 - 1), cr = block_nonzero(2, x0 / 2, y0 / 2, log2 - 1);
  if (depth == 0 || parent_cb) cw.decision(kCtxCbfChroma + depth, cb);
  if (depth == 0 || parent_cr) cw.decision(kCtxCbfChroma + depth, cr);
  if (split) {
    const int half = 1 << (log2 - 1);
    write_tu_tree(cw, x0, y0, log2 - 1, depth + 1, intra, intra_mode, cb, cr);
    write_tu_tree(cw, x0 + half, y0, log2 - 1, depth + 1, intra, intra_mode, cb, cr);
    write_tu_tree(cw, x0, y0 + half, log2 - 1, depth + 1, intra, intra_mode, cb, cr);
    write_tu_tree(cw, x0 + half, y0 + half, log2 - 1, depth + 1, intra, intra_mode, cb, cr);
    return;
  }
  const bool luma = block_nonzero(0, x0, y0, log2);
  if (intra || depth != 0 || cb || cr) cw.decision(kCtxCbfLuma + (depth == 0 ? 1 : 0), luma);
  if ((luma || cb || cr) && !delta_coded_) {
    const int delta = ctu_qp_ - qp_pred_;
    const int a = iabs(delta);
    for (int k = 0; k < imin(a, 5); ++k) cw.decision(kCtxCuQpDelta + (k == 0 ? 0 : 1), 1);
    if (a < 5) {
      cw.decision(kCtxCuQpDelta + (a == 0 ? 0 : 1), 0);
    } else {
      uint32_t v = static_cast<uint32_t>(a - 5);
      int k = 0;
      while (v >= (1u << k)) {
        cw.bypass(1);
        v -= 1u << k;
        ++k;
      }
      cw.bypass(0);
      cw.bypass_bits(v, k);
    }
    if (a > 0) cw.bypass(delta < 0 ? 1 : 0);
    delta_coded_ = true;
  }
  if (luma) write_residual(cw, 0, x0, y0, log2, intra, intra_mode);
  if (cb) write_residual(cw, 1, x0 / 2, y0 / 2, log2 - 1, intra, intra_mode);
  if (cr) write_residual(cw, 2, x0 / 2, y0 / 2, log2 - 1, intra, intra_mode);
}

void PictureEncoder::write_pu(CabacWriter& cw, int x0, int y0, const CuInfo& cu) {
  if (!cu.skip) cw.decision(kCtxMergeFlag, cu.merge ? 1 : 0);
  if (cu.merge) {
    // merge_idx: TR, cMax 4, the first bin with a context
    const int idx = cu.merge_idx;
    cw.decision(kCtxMergeIdx, idx > 0);
    if (idx > 0) {
      for (int k = 1; k < idx; ++k) cw.bypass(1);
      if (idx < 4) cw.bypass(0);
    }
    return;
  }
  if (slice_type_ == kSliceB) {
    const int depth = cell(x0, y0).depth;
    if (cu.pred_idc == kPredBi) {
      cw.decision(kCtxInterPredIdc + depth, 1);
    } else {
      cw.decision(kCtxInterPredIdc + depth, 0);
      cw.decision(kCtxInterPredIdc + 4, cu.pred_idc == kPredL1);
    }
  }
  for (int l = 0; l < 2; ++l) {
    if (!(l == 0 ? cu.pred_idc != kPredL1 : cu.pred_idc != kPredL0)) continue;
    const int num = num_ref_[l];
    if (num > 1) {
      const int r = cu.ref_idx[l], max = num - 1;
      for (int k = 0; k < imin(r, 2); ++k) cw.decision(kCtxRefIdx + k, 1);
      if (r < 2) {
        if (r < max) cw.decision(kCtxRefIdx + r, 0);
      } else {
        for (int k = 2; k < r; ++k) cw.bypass(1);
        if (r < max) cw.bypass(0);
      }
    }
    // mvd_coding (mvd_l1_zero_flag is 0)
    const int mx = cu.mvd[l][0], my = cu.mvd[l][1];
    const int ax = iabs(mx), ay = iabs(my);
    cw.decision(kCtxMvdGreater0, ax > 0);
    cw.decision(kCtxMvdGreater0, ay > 0);
    if (ax > 0) cw.decision(kCtxMvdGreater1, ax > 1);
    if (ay > 0) cw.decision(kCtxMvdGreater1, ay > 1);
    for (int comp = 0; comp < 2; ++comp) {
      const int a = comp == 0 ? ax : ay;
      if (a == 0) continue;
      if (a > 1) {
        uint32_t v = static_cast<uint32_t>(a - 2);
        int k = 1;
        while (v >= (1u << k)) {
          cw.bypass(1);
          v -= 1u << k;
          ++k;
        }
        cw.bypass(0);
        cw.bypass_bits(v, k);
      }
      cw.bypass((comp == 0 ? mx : my) < 0 ? 1 : 0);
    }
    cw.decision(kCtxMvpFlag, cu.mvp[l]);
  }
}

void PictureEncoder::write_cu(CabacWriter& cw, int x0, int y0, int log2) {
  const Cell& k = cell(x0, y0);
  const CuInfo& cu = k.cu;
  if (slice_type_ != kSliceI) {
    int inc = 0;
    if (available(x0, y0, x0 - 1, y0) && (cell(x0 - 1, y0).mode & kModeSkip)) ++inc;
    if (available(x0, y0, x0, y0 - 1) && (cell(x0, y0 - 1).mode & kModeSkip)) ++inc;
    cw.decision(kCtxSkip + inc, cu.skip ? 1 : 0);
  }
  // statistics
  if (stats_) {
    const uint64_t units = uint64_t{1} << (2 * (log2 - 3));
    stats_->units += units;
    stats_->qp_sum += static_cast<uint64_t>(ctu_qp_ < 0 ? 0 : ctu_qp_) * units;
    if (cu.intra) {
      stats_->intra += units;
    } else {
      if (cu.skip) stats_->skipped += units;
      else stats_->inter += units;
      const int l = (k.mv.pred_flags & 1) ? 0 : 1;
      stats_->vector_x_sum += static_cast<int64_t>(k.mv.mv[l][0]) * static_cast<int64_t>(units);
      stats_->vector_y_sum += static_cast<int64_t>(k.mv.mv[l][1]) * static_cast<int64_t>(units);
    }
  }
  if (cu.skip) {
    write_pu(cw, x0, y0, cu);
    return;
  }
  if (slice_type_ != kSliceI) cw.decision(kCtxPredMode, cu.intra ? 1 : 0);
  if (!cu.intra || log2 == pic_->seq.log2_min_cb) cw.decision(kCtxPartMode, 1);   // PART_2Nx2N
  if (cu.intra) {
    int list[3];
    mpm_list(x0, y0, list);
    const int mode = cu.intra_mode;
    int idx = -1;
    for (int i = 0; i < 3; ++i)
      if (list[i] == mode) idx = i;
    cw.decision(kCtxPrevIntraLuma, idx >= 0);
    if (idx >= 0) {
      cw.bypass(idx > 0);
      if (idx > 0) cw.bypass(idx > 1);
    } else {
      int s[3] = {list[0], list[1], list[2]};
      if (s[0] > s[1]) { const int t = s[0]; s[0] = s[1]; s[1] = t; }
      if (s[0] > s[2]) { const int t = s[0]; s[0] = s[2]; s[2] = t; }
      if (s[1] > s[2]) { const int t = s[1]; s[1] = s[2]; s[2] = t; }
      int rem = mode;
      for (int i = 2; i >= 0; --i)
        if (rem > s[i]) --rem;
      cw.bypass_bits(static_cast<uint32_t>(rem), 5);
    }
    cw.decision(kCtxChromaPred, 0);   // intra_chroma_pred_mode 4: that of luma
    write_tu_tree(cw, x0, y0, log2, 0, true, mode, true, true);
    return;
  }
  write_pu(cw, x0, y0, cu);
  const int n = 1 << log2;
  const bool any = block_nonzero(0, x0, y0, log2) || block_nonzero(1, x0 / 2, y0 / 2, log2 - 1) || block_nonzero(2, x0 / 2, y0 / 2, log2 - 1);
  (void)n;
  if (!cu.merge) cw.decision(kCtxRqtRootCbf, any ? 1 : 0);
  if (any) write_tu_tree(cw, x0, y0, log2, 0, false, 1, true, true);
}

void PictureEncoder::write_quadtree(CabacWriter& cw, int x0, int y0, int log2, int depth) {
  const H265EncSequence& s = pic_->seq;
  const int size = 1 << log2;
  bool split;
  if (x0 + size <= W_ && y0 + size <= H_ && log2 > s.log2_min_cb) {
    int inc = 0;
    if (available(x0, y0, x0 - 1, y0) && cell(x0 - 1, y0).depth > depth) ++inc;
    if (available(x0, y0, x0, y0 - 1) && cell(x0, y0 - 1).depth > depth) ++inc;
    split = cell(x0, y0).log2 < log2;
    cw.decision(kCtxSplitCu + inc, split ? 1 : 0);
  } else {
    split = log2 > s.log2_min_cb;
  }
  if (split) {
    const int half = size >> 1;
    const int x1 = x0 + half, y1 = y0 + half;
    write_quadtree(cw, x0, y0, log2 - 1, depth + 1);
    if (x1 < W_) write_quadtree(cw, x1, y0, log2 - 1, depth + 1);
    if (y1 < H_) write_quadtree(cw, x0, y1, log2 - 1, depth + 1);
    if (x1 < W_ && y1 < H_) write_quadtree(cw, x1, y1, log2 - 1, depth + 1);
  } else {
    write_cu(cw, x0, y0, log2);
  }
}

void PictureEncoder::write_sao(CabacWriter& cw, int ctb, bool sao_luma, bool sao_chroma) {
  const EncSao& p = ctb_sao_[static_cast<uint64_t>(ctb)];
  const int rx = ctb % ctb_w_, ry = ctb / ctb_w_;
  bool merge_left = false;
  if (rx > 0 && ctb - 1 >= slice_start_) {
    merge_left = p.merge_left;
    cw.decision(kCtxSaoMerge, merge_left);
  }
  if (ry > 0 && !merge_left && ctb - ctb_w_ >= slice_start_) {
    cw.decision(kCtxSaoMerge, p.merge_up);
    if (p.merge_up) return;
  }
  if (merge_left) return;
  const int bd10 = bd_ < 10 ? bd_ : 10;
  const int c_max = (1 << (bd10 - 5)) - 1;
  for (int c = 0; c < 3; ++c) {
    if (!((sao_luma && c == 0) || (sao_chroma && c > 0))) continue;
    if (c < 2) {
      cw.decision(kCtxSaoType, p.type[c] != 0);
      if (p.type[c]) cw.bypass(p.type[c] == 2);
    }
    if (p.type[c] == 0) continue;
    for (int i = 0; i < 4; ++i) {
      const int a = iabs(p.offset[c][i]);
      for (int k = 0; k < a; ++k) cw.bypass(1);
      if (a < c_max) cw.bypass(0);
    }
    if (p.type[c] == 1) {
      for (int i = 0; i < 4; ++i)
        if (p.offset[c][i] != 0) cw.bypass(p.offset[c][i] < 0);
      cw.bypass_bits(p.band[c], 5);
    } else if (c < 2) {
      cw.bypass_bits(p.eo_class[c], 2);
    }
  }
}

void PictureEncoder::write_ctu(CabacWriter& cw, int ctb, bool sao_luma, bool sao_chroma) {
  if (sao_luma || sao_chroma) write_sao(cw, ctb, sao_luma, sao_chroma);
  ctu_qp_ = ctb_qp_[static_cast<uint64_t>(ctb)];
  qp_pred_ = qp_prev_;
  delta_coded_ = false;
  const int x = (ctb % ctb_w_) << log2_ctb_, y = (ctb / ctb_w_) << log2_ctb_;
  write_quadtree(cw, x, y, log2_ctb_, 0);
  if (delta_coded_) qp_prev_ = ctu_qp_;
}


// ---------------------------------------------------------------------------------------------------------------------------------------------------------------
// Slices

bool PictureEncoder::slice_header(BitWriter& w, int slice, bool sao, bool drift_check) {
  const H265EncSequence& seq = pic_->seq;
  const uint32_t first_ctb = slice_bounds_[static_cast<uint64_t>(slice)];
  const bool idr = pic_->type == H265EncFrameType::Idr;
  w.bit(first_ctb == 0);
  if (idr) w.bit(0);   // no_output_of_prior_pics_flag
  w.ue(pic_->pps_id);
  if (first_ctb != 0) w.put(first_ctb, ceil_log2(static_cast<uint32_t>(ctb_w_ * ctb_h_)));
  w.ue(static_cast<uint32_t>(slice_type_));
  if (!idr) {
    const uint32_t max_lsb = 1u << seq.log2_max_poc_lsb;
    w.put(static_cast<uint32_t>(pic_->poc) & (max_lsb - 1), seq.log2_max_poc_lsb);
    w.bit(0);   // short_term_ref_pic_set_sps_flag
    const uint64_t start = w.bits();
    // st_ref_pic_set(num_short_term_ref_pic_sets = 0): no inter-set prediction
    w.ue(static_cast<uint32_t>(n_neg_));
    w.ue(static_cast<uint32_t>(n_pos_));
    int prev = pic_->poc;
    for (int i = 0; i < n_neg_; ++i) {
      const H265EncReference& r = pic_->dpb[static_cast<uint64_t>(neg_[i])];
      w.ue(static_cast<uint32_t>(prev - r.poc - 1));
      w.bit(r.used_by_current);
      prev = r.poc;
    }
    prev = pic_->poc;
    for (int i = 0; i < n_pos_; ++i) {
      const H265EncReference& r = pic_->dpb[static_cast<uint64_t>(pos_[i])];
      w.ue(static_cast<uint32_t>(r.poc - prev - 1));
      w.bit(r.used_by_current);
      prev = r.poc;
    }
    st_rps_bits_ = static_cast<int>(w.bits() - start);
    if (seq.long_term_refs) {
      w.ue(static_cast<uint32_t>(n_ltall_));
      const int32_t mask = static_cast<int32_t>(max_lsb - 1);
      const int32_t cur_msb = pic_->poc - (pic_->poc & mask);
      int prev_cycle = 0;
      for (int i = 0; i < n_ltall_; ++i) {
        const H265EncReference& r = pic_->dpb[static_cast<uint64_t>(lt_[i])];
        const int32_t lsb = r.poc & mask;
        const int cycle = static_cast<int>((cur_msb - (r.poc - lsb)) / static_cast<int32_t>(max_lsb));
        w.put(static_cast<uint32_t>(lsb), seq.log2_max_poc_lsb);
        w.bit(r.used_by_current);
        w.bit(1);   // delta_poc_msb_present_flag
        w.ue(static_cast<uint32_t>(cycle - (i == 0 ? 0 : prev_cycle)));
        prev_cycle = cycle;
      }
    }
  }
  if (seq.sao) {
    w.bit(sao);
    w.bit(sao);
  }
  if (slice_type_ != kSliceI) {
    w.bit(1);   // num_ref_idx_active_override_flag
    w.ue(static_cast<uint32_t>(num_ref_[0] - 1));
    if (slice_type_ == kSliceB) w.ue(static_cast<uint32_t>(num_ref_[1] - 1));
    const int total = n_before_ + n_after_ + n_lt_;
    if (total > 1) {
      const int bits = ceil_log2(static_cast<uint32_t>(total));
      for (int l = 0; l < (slice_type_ == kSliceB ? 2 : 1); ++l) {
        w.bit(modify_[l]);
        if (modify_[l])
          for (int i = 0; i < num_ref_[l]; ++i) w.put(static_cast<uint32_t>(list_entry_[l][i]), bits);
      }
    }
    if (slice_type_ == kSliceB) w.bit(0);   // mvd_l1_zero_flag
    w.ue(0);                                 // five_minus_max_num_merge_cand
  }
  w.se(pic_->qp - 26);
  if (seq.loop_filter_across_slices && (sao || !drift_check)) w.bit(1);
  w.bit(1);   // byte_alignment()
  w.align_zero();
  return w.ok();
}

bool PictureEncoder::write_slices(ByteBuffer* out, List<H265EncSlice>* slices, bool sao, bool drift_check) {
  slices->clear();
  const int init_type = slice_type_ == kSliceI ? 0 : slice_type_ == kSliceP ? 1 : 2;
  const int nal_type = pic_->type == H265EncFrameType::Idr ? kIdrWRadl : pic_->reference ? kTrailR : kTrailN;
  for (uint32_t s = 0; s < slice_count_; ++s) {
    BitWriter w(allocator_);
    if (!slice_header(w, static_cast<int>(s), sao, drift_check)) return false;
    const uint64_t header_bytes = w.bits() / 8;
    CabacWriter cw;
    cw.start(&w, init_type, pic_->qp);
    slice_start_ = static_cast<int>(slice_bounds_[s]);
    qp_prev_ = pic_->qp;
    const uint32_t end = slice_bounds_[s + 1];
    for (uint32_t ctb = slice_bounds_[s]; ctb < end; ++ctb) {
      write_ctu(cw, static_cast<int>(ctb), sao, sao);
      cw.terminate(ctb + 1 == end ? 1 : 0);   // end_of_slice_segment_flag
    }
    w.align_zero();
    if (!w.ok()) return false;
    H265EncSlice rec;
    rec.offset = out->size();
    uint64_t hb = 0;
    if (!put_nal(out, nal_type, pic_->temporal_id, w.bytes(), header_bytes, &hb)) return false;
    rec.size = out->size() - rec.offset;
    rec.header_bits = hb * 8;
    if (!slices->push_back(rec)) return false;
  }
  return true;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------
// The reconstruction: the in-tree decoder, driven as DXVA drives it

namespace {
int picture_sample(const Picture& p, int c, uint64_t i) { return p.deep ? samples<uint16_t>(p.plane[c])[i] : p.plane[c][i]; }
}

bool PictureEncoder::decode(ConstByteView stream, ArrayView<const H265EncSlice> slices, bool drift_check) {
  if (!core_) return false;
  const H265EncSequence& q = pic_->seq;
  Sps sps;
  sps.valid = true;
  sps.width = W_;
  sps.height = H_;
  sps.bit_depth_luma = sps.bit_depth_chroma = bd_;
  sps.log2_max_poc_lsb = q.log2_max_poc_lsb;
  sps.max_dec_pic_buffering[0] = 16;
  sps.log2_min_cb = q.log2_min_cb;
  sps.log2_ctb = q.log2_ctb;
  sps.log2_min_tb = q.log2_min_tb;
  sps.log2_max_tb = q.log2_max_tb;
  sps.max_th_depth_inter = q.max_th_depth_inter;
  sps.max_th_depth_intra = q.max_th_depth_intra;
  sps.amp = q.amp;
  sps.sao = q.sao;
  sps.long_term_ref_pics_present = q.long_term_refs;
  sps.scaling.set_default();
  sps.ctb_size = 1 << q.log2_ctb;
  sps.pic_w_ctb = ctb_w_;
  sps.pic_h_ctb = ctb_h_;
  Pps pps;
  pps.valid = true;
  pps.id = static_cast<int>(pic_->pps_id);
  pps.init_qp = 26;
  pps.constrained_intra_pred = q.constrained_intra_pred;
  pps.transform_skip = q.transform_skip;
  pps.cu_qp_delta_enabled = true;
  pps.loop_filter_across_slices = q.loop_filter_across_slices;
  pps.deblocking_control_present = true;
  pps.deblocking_disabled = drift_check;
  pps.lists_modification_present = true;
  pps.scaling.set_default();
  core_->external_prepare(sps, pps);
  Core::ExternalPicture e;
  e.target = &target_;
  e.poc = pic_->poc;
  e.st_rps_bits = pic_->type == H265EncFrameType::Idr ? 0 : st_rps_bits_;
  int next = 0;
  auto make = [&](int dpb_index, bool long_term) -> Picture* {
    if (next >= static_cast<int>(kH265EncMaxReferences)) return nullptr;
    Picture* p = &refpic_[next++];
    const H265EncReference& r = pic_->dpb[static_cast<uint64_t>(dpb_index)];
    if (!core_->external_grey(p, r.poc)) return nullptr;
    p->is_long = long_term;
    for (int c = 0; c < 3; ++c) {
      const ArrayView<const uint16_t> s = c == 0 ? r.picture.y : c == 1 ? r.picture.u : r.picture.v;
      const uint64_t count = c == 0 ? static_cast<uint64_t>(W_) * H_ : static_cast<uint64_t>(cw_) * ch_;
      if (p->deep) {
        uint16_t* d = samples<uint16_t>(p->plane[c]);
        for (uint64_t i = 0; i < count; ++i) d[i] = s[i];
      } else {
        for (uint64_t i = 0; i < count; ++i) p->plane[c][i] = static_cast<uint8_t>(s[i]);
      }
    }
    return p;
  };
  for (int i = 0; i < n_before_; ++i)
    if (!(e.st_before[i] = make(st_before_[i], false))) return false;
  for (int i = 0; i < n_after_; ++i)
    if (!(e.st_after[i] = make(st_after_[i], false))) return false;
  for (int i = 0; i < n_lt_; ++i)
    if (!(e.lt_curr[i] = make(lt_curr_[i], true))) return false;
  e.st_before_count = n_before_;
  e.st_after_count = n_after_;
  e.lt_count = n_lt_;
  if (!core_->external_begin(e)) return false;
  bool ok = true;
  for (const H265EncSlice& s : slices) ok = core_->external_slice(stream.subview(s.offset + 4, s.size - 4)) && ok;
  return core_->external_end() && ok;
}

// SAO parameters per coding tree block from the deblocked picture (target_): band and edge offsets that lower the squared error most, against their bits.
void PictureEncoder::choose_sao() {
  const int bd10 = bd_ < 10 ? bd_ : 10;
  const int c_max = (1 << (bd10 - 5)) - 1;
  const int ctb = 1 << log2_ctb_;
  static const int kDx[4][2] = {{-1, 1}, {0, 0}, {-1, 1}, {1, -1}};
  static const int kDy[4][2] = {{0, 0}, {-1, 1}, {-1, 1}, {-1, 1}};
  for (int r = 0; r < ctb_h_; ++r)
    for (int q = 0; q < ctb_w_; ++q) {
      const int index = r * ctb_w_ + q;
      EncSao p;
      const int64_t lambda = kLambda[imin(63, ctb_qp_[static_cast<uint64_t>(index)] + 12)];
      int64_t gain_band[3] = {}, gain_edge[3][4] = {};
      uint8_t band_pos[3] = {};
      int8_t band_off[3][4] = {}, edge_off[3][4][4] = {};
      for (int c = 0; c < 3; ++c) {
        const int f = c ? 2 : 1, size = ctb / f, pw = c ? cw_ : W_, ph = c ? ch_ : H_;
        const int x0 = q * size, y0 = r * size, x1 = imin(pw, x0 + size), y1 = imin(ph, y0 + size);
        const ArrayView<const uint16_t> src = c == 0 ? src_.y : c == 1 ? src_.u : src_.v;
        int64_t bsum[32] = {}, bcount[32] = {};
        int64_t esum[4][5] = {}, ecount[4][5] = {};
        for (int y = y0; y < y1; ++y)
          for (int x = x0; x < x1; ++x) {
            const uint64_t at = static_cast<uint64_t>(y) * pw + x;
            const int d = picture_sample(target_, c, at);
            const int diff = src[at] - d;
            const int band = d >> (bd_ - 5);
            bsum[band] += diff;
            ++bcount[band];
            if (x == 0 || y == 0 || x == pw - 1 || y == ph - 1) continue;
            for (int k = 0; k < 4; ++k) {
              const int a = picture_sample(target_, c, static_cast<uint64_t>(y + kDy[k][0]) * pw + x + kDx[k][0]);
              const int b = picture_sample(target_, c, static_cast<uint64_t>(y + kDy[k][1]) * pw + x + kDx[k][1]);
              int e = 2 + (d > a) - (d < a) + (d > b) - (d < b);
              if (e <= 2) e = e == 2 ? 0 : e + 1;
              esum[k][e] += diff;
              ++ecount[k][e];
            }
          }
        auto best_offset = [&](int64_t sum, int64_t count, int lo, int hi, int64_t* gain) {
          *gain = 0;
          if (count == 0) return 0;
          const int64_t o = (sum >= 0 ? sum + count / 2 : sum - count / 2) / count;
          const int off = clip3(lo, hi, static_cast<int>(o < -1000 ? -1000 : o > 1000 ? 1000 : o));
          *gain = int64_t{off} * off * count - 2 * int64_t{off} * sum;   // the change of the squared error
          return off;
        };
        int64_t best = 0;
        for (int start = 0; start < 32; ++start) {
          int64_t g = 0;
          int8_t offs[4];
          for (int k = 0; k < 4; ++k) {
            int64_t gk;
            offs[k] = static_cast<int8_t>(best_offset(bsum[(start + k) & 31], bcount[(start + k) & 31], -c_max, c_max, &gk));
            g += gk;
          }
          if (start == 0 || g < best) {
            best = g;
            band_pos[c] = static_cast<uint8_t>(start);
            for (int k = 0; k < 4; ++k) band_off[c][k] = offs[k];
          }
        }
        gain_band[c] = best;
        for (int k = 0; k < 4; ++k) {
          int64_t g = 0;
          for (int cat = 1; cat <= 4; ++cat) {
            int64_t gk;
            const int off = best_offset(esum[k][cat], ecount[k][cat], cat <= 2 ? 0 : -c_max, cat <= 2 ? c_max : 0, &gk);
            edge_off[c][k][cat - 1] = static_cast<int8_t>(off);
            g += gk;
          }
          gain_edge[c][k] = g;
        }
      }
      const int64_t scale = sse_scale_;
      {
        int64_t best = 0;
        const int64_t band = gain_band[0] * scale + lambda * 14;
        if (band < best) {
          best = band;
          p.type[0] = 1;
          p.band[0] = band_pos[0];
          for (int k = 0; k < 4; ++k) p.offset[0][k] = band_off[0][k];
        }
        for (int k = 0; k < 4; ++k) {
          const int64_t edge = gain_edge[0][k] * scale + lambda * 12;
          if (edge < best) {
            best = edge;
            p.type[0] = 2;
            p.eo_class[0] = static_cast<uint8_t>(k);
            for (int i = 0; i < 4; ++i) p.offset[0][i] = edge_off[0][k][i];
          }
        }
      }
      {
        int64_t best = 0;
        const int64_t band = (gain_band[1] + gain_band[2]) * scale + lambda * 26;
        if (band < best) {
          best = band;
          for (int c = 1; c < 3; ++c) {
            p.type[c] = 1;
            p.band[c] = band_pos[c];
            for (int k = 0; k < 4; ++k) p.offset[c][k] = band_off[c][k];
          }
        }
        for (int k = 0; k < 4; ++k) {
          const int64_t edge = (gain_edge[1][k] + gain_edge[2][k]) * scale + lambda * 22;
          if (edge < best) {
            best = edge;
            for (int c = 1; c < 3; ++c) {
              p.type[c] = 2;
              p.eo_class[c] = static_cast<uint8_t>(k);
              for (int i = 0; i < 4; ++i) p.offset[c][i] = edge_off[c][k][i];
            }
          }
        }
      }
      // the same as the left or upper block of the slice: merged
      int slice_first = 0;
      for (uint32_t s = 0; s < slice_count_; ++s)
        if (static_cast<int>(slice_bounds_[s]) <= index) slice_first = static_cast<int>(slice_bounds_[s]);
      if (q > 0 && index - 1 >= slice_first && p.same(ctb_sao_[static_cast<uint64_t>(index - 1)])) p.merge_left = true;
      else if (r > 0 && index - ctb_w_ >= slice_first && p.same(ctb_sao_[static_cast<uint64_t>(index - ctb_w_)])) p.merge_up = true;
      ctb_sao_[static_cast<uint64_t>(index)] = p;
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------
// The picture

H265EncResult PictureEncoder::prepare(const H265EncPicture& pic, const EncPlanes16& src) {
  const H265EncSequence& seq = pic.seq;
  pic_ = &pic;
  src_ = src;
  W_ = static_cast<int>(seq.width);
  H_ = static_cast<int>(seq.height);
  cw_ = W_ / 2;
  ch_ = H_ / 2;
  bd_ = seq.bit_depth;
  qp_bd_ = 6 * (bd_ - 8);
  sse_scale_ = int64_t{256} >> (2 * (bd_ - 8));
  log2_ctb_ = seq.log2_ctb;
  ctb_w_ = (W_ + (1 << log2_ctb_) - 1) >> log2_ctb_;
  ctb_h_ = (H_ + (1 << log2_ctb_) - 1) >> log2_ctb_;
  w8_ = ctb_w_ << (log2_ctb_ - 3);
  h8_ = ctb_h_ << (log2_ctb_ - 3);
  const uint64_t ctbs = static_cast<uint64_t>(ctb_w_) * ctb_h_;
  if (pic.pps_id > 63 || pic.temporal_id > 6 || pic.poc < 0 || pic.qp < -qp_bd_ || pic.qp > 51 || static_cast<uint8_t>(pic.type) > 3)
    return H265EncResult::PictureControlNotSupported;
  if (pic.type == H265EncFrameType::Idr && (pic.poc != 0 || pic.temporal_id != 0)) return H265EncResult::PictureControlNotSupported;
  slice_type_ = pic.type == H265EncFrameType::B ? kSliceB : pic.type == H265EncFrameType::P ? kSliceP : kSliceI;
  // the reference picture set
  n_neg_ = n_pos_ = n_ltall_ = n_before_ = n_after_ = n_lt_ = 0;
  if (pic.type != H265EncFrameType::Idr) {
    if (pic.dpb.size() > kH265EncMaxReferences) return H265EncResult::PictureControlNotSupported;
    const int32_t max_lsb = 1 << seq.log2_max_poc_lsb;
    const int32_t cur_msb = pic.poc - (pic.poc & (max_lsb - 1));
    for (uint64_t k = 0; k < pic.dpb.size(); ++k) {
      const H265EncReference& r = pic.dpb[k];
      if (r.picture.width != seq.width || r.picture.height != seq.height || !r.picture.fits()) return H265EncResult::InvalidReferences;
      if (r.poc == pic.poc || r.poc < 0) return H265EncResult::PictureControlNotSupported;
      for (uint64_t j = 0; j < k; ++j)
        if (pic.dpb[j].poc == r.poc) return H265EncResult::PictureControlNotSupported;
      if (r.long_term) {
        if (!seq.long_term_refs || r.poc > pic.poc) return H265EncResult::PictureControlNotSupported;
        lt_[n_ltall_++] = static_cast<int>(k);
      } else if (r.poc < pic.poc) {
        if (pic.poc - r.poc > 32768) return H265EncResult::PictureControlNotSupported;
        neg_[n_neg_++] = static_cast<int>(k);
      } else {
        if (r.poc - pic.poc > 32768) return H265EncResult::PictureControlNotSupported;
        pos_[n_pos_++] = static_cast<int>(k);
      }
    }
    // order: the negative ones closest first, the positive ones closest first, the long-term ones by their MSB cycle
    auto sort = [&](int* a, int n, auto less) {
      for (int i = 1; i < n; ++i)
        for (int j = i; j > 0 && less(a[j], a[j - 1]); --j) {
          const int t = a[j];
          a[j] = a[j - 1];
          a[j - 1] = t;
        }
    };
    sort(neg_, n_neg_, [&](int a, int b) { return pic.dpb[static_cast<uint64_t>(a)].poc > pic.dpb[static_cast<uint64_t>(b)].poc; });
    sort(pos_, n_pos_, [&](int a, int b) { return pic.dpb[static_cast<uint64_t>(a)].poc < pic.dpb[static_cast<uint64_t>(b)].poc; });
    auto cycle = [&](int k) {
      const int32_t p = pic.dpb[static_cast<uint64_t>(k)].poc;
      return (cur_msb - (p - (p & (max_lsb - 1)))) / max_lsb;
    };
    sort(lt_, n_ltall_, [&](int a, int b) { return cycle(a) < cycle(b); });
    for (int i = 0; i < n_neg_; ++i)
      if (pic.dpb[static_cast<uint64_t>(neg_[i])].used_by_current) {
        if (n_before_ == 8) return H265EncResult::PictureControlNotSupported;
        st_before_[n_before_++] = neg_[i];
      }
    for (int i = 0; i < n_pos_; ++i)
      if (pic.dpb[static_cast<uint64_t>(pos_[i])].used_by_current) {
        if (n_after_ == 8) return H265EncResult::PictureControlNotSupported;
        st_after_[n_after_++] = pos_[i];
      }
    for (int i = 0; i < n_ltall_; ++i)
      if (pic.dpb[static_cast<uint64_t>(lt_[i])].used_by_current) {
        if (n_lt_ == 8) return H265EncResult::PictureControlNotSupported;
        lt_curr_[n_lt_++] = lt_[i];
      }
  }
  // the lists: RefPicListTemp0 is before, after, long-term; RefPicListTemp1 after, before, long-term (8.3.4)
  const int total = n_before_ + n_after_ + n_lt_;
  num_ref_[0] = num_ref_[1] = 0;
  modify_[0] = modify_[1] = false;
  const int lists = slice_type_ == kSliceB ? 2 : slice_type_ == kSliceP ? 1 : 0;
  for (int l = 0; l < lists; ++l) {
    int temp[24];
    int nt = 0;
    const int* first = l == 0 ? st_before_ : st_after_;
    const int* second = l == 0 ? st_after_ : st_before_;
    const int nf = l == 0 ? n_before_ : n_after_, ns = l == 0 ? n_after_ : n_before_;
    for (int i = 0; i < nf; ++i) temp[nt++] = first[i];
    for (int i = 0; i < ns; ++i) temp[nt++] = second[i];
    for (int i = 0; i < n_lt_; ++i) temp[nt++] = lt_curr_[i];
    const ArrayView<const uint8_t> final_list = pic.list[l];
    if (final_list.empty() || final_list.size() > 15 || total == 0) return H265EncResult::InvalidReferences;
    num_ref_[l] = static_cast<int>(final_list.size());
    for (int i = 0; i < num_ref_[l]; ++i) {
      const int d = final_list[static_cast<uint64_t>(i)];
      if (d >= static_cast<int>(pic.dpb.size()) || !pic.dpb[static_cast<uint64_t>(d)].used_by_current) return H265EncResult::InvalidReferences;
      int entry = -1;
      for (int k = 0; k < nt && entry < 0; ++k)
        if (temp[k] == d) entry = k;
      if (entry < 0) return H265EncResult::InvalidReferences;
      list_entry_[l][i] = entry;
      if (temp[i % nt] != d) modify_[l] = true;
      list_[l][i] = {d, pic.dpb[static_cast<uint64_t>(d)].poc, pic.dpb[static_cast<uint64_t>(d)].long_term};
    }
    const ArrayView<const uint32_t> given = pic.modification[l];
    if (!given.empty()) {
      if (given.size() != static_cast<uint64_t>(num_ref_[l])) return H265EncResult::PictureControlNotSupported;
      for (int i = 0; i < num_ref_[l]; ++i) {
        const uint32_t e = given[static_cast<uint64_t>(i)];
        if (e >= static_cast<uint32_t>(total) || temp[e] != final_list[static_cast<uint64_t>(i)]) return H265EncResult::PictureControlNotSupported;
        list_entry_[l][i] = static_cast<int>(e);
      }
      modify_[l] = total > 1;
    }
    if (modify_[l] && total <= 1) return H265EncResult::InvalidReferences;
  }
  // slices
  const uint32_t count = H265PictureEncoder::slice_count(pic.slice_mode, pic.slice_value, static_cast<uint32_t>(ctb_w_), static_cast<uint32_t>(ctb_h_));
  if (count == 0) return H265EncResult::SubregionLayoutNotSupported;
  slice_bounds_.clear();
  for (uint32_t s = 0; s <= count; ++s) {
    uint64_t start;
    switch (pic.slice_mode) {
      case H265EncSliceMode::CtbsPerSlice: start = uint64_t{s} * pic.slice_value; break;
      case H265EncSliceMode::RowsPerSlice: start = uint64_t{s} * pic.slice_value * static_cast<uint32_t>(ctb_w_); break;
      case H265EncSliceMode::SlicesPerFrame: start = uint64_t{s} * ctbs / count; break;
      default: start = s == 0 ? 0 : ctbs;
    }
    if (!slice_bounds_.push_back(static_cast<uint32_t>(start < ctbs ? start : ctbs))) return H265EncResult::NoMemory;
  }
  slice_count_ = count;
  // the planes and maps
  if (!rec_[0].reset(static_cast<uint64_t>(W_) * H_) || !rec_[1].reset(static_cast<uint64_t>(cw_) * ch_) || !rec_[2].reset(static_cast<uint64_t>(cw_) * ch_) ||
      !coef_[0].reset(static_cast<uint64_t>(W_) * H_) || !coef_[1].reset(static_cast<uint64_t>(cw_) * ch_) || !coef_[2].reset(static_cast<uint64_t>(cw_) * ch_) ||
      !cells_.reset(static_cast<uint64_t>(w8_) * h8_) || !ctb_qp_.reset(ctbs) || !ctb_sao_.reset(ctbs))
    return H265EncResult::NoMemory;
  // the quantizers of the blocks (before the limits of cu_qp_delta, which the decisions apply)
  if (!pic.qp_map.empty() && pic.qp_map.size() != ctbs) return H265EncResult::PictureControlNotSupported;
  for (uint64_t i = 0; i < ctbs; ++i) {
    int q = pic.qp;
    if (!pic.qp_map.empty()) q = pic.qp_map_absolute ? pic.qp_map[i] : q + pic.qp_map[i];
    ctb_qp_[i] = static_cast<int8_t>(clip3(-qp_bd_, 51, q));
  }
  // motion estimation into every reference the lists name (8-bit lumas)
  for (int& m : motion_slot_) m = -1;
  if (lists) {
    int refs[kH265EncMaxReferences];
    int nr = 0;
    for (int l = 0; l < lists; ++l)
      for (int i = 0; i < num_ref_[l]; ++i) {
        bool seen = false;
        for (int k = 0; k < nr; ++k) seen = seen || refs[k] == list_[l][i].dpb;
        if (!seen) refs[nr++] = list_[l][i].dpb;
      }
    const uint64_t luma = static_cast<uint64_t>(W_) * H_;
    motion_blocks_ = ((W_ + 15) / 16) * ((H_ + 15) / 16);
    if (!luma8_.reset(luma * 2) || !motion_.reset(static_cast<uint64_t>(motion_blocks_) * nr)) return H265EncResult::NoMemory;
    const int shift = bd_ - 8;
    for (uint64_t i = 0; i < luma; ++i) luma8_[i] = static_cast<uint8_t>(src.y[i] >> shift);
    for (int k = 0; k < nr; ++k) {
      const ArrayView<const uint16_t> ry = pic.dpb[static_cast<uint64_t>(refs[k])].picture.y;
      for (uint64_t i = 0; i < luma; ++i) luma8_[luma + i] = static_cast<uint8_t>(ry[i] >> shift);
      const LumaPlane cur{ConstByteView(luma8_.view().subview(0, luma)), W_, H_, W_};
      const LumaPlane ref{ConstByteView(luma8_.view().subview(luma, luma)), W_, H_, W_};
      if (!estimate_motion(cur, ref, 16, ArrayView<const MotionVector>(), motion_.view().subview(static_cast<uint64_t>(k) * motion_blocks_, motion_blocks_)))
        return H265EncResult::NoMemory;
      motion_slot_[refs[k]] = k;
    }
  }
  return H265EncResult::Ok;
}

H265EncResult PictureEncoder::encode(const H265EncPicture& pic, const EncPlanes16& src, const EncWritablePlanes16& rec, ByteBuffer* out, List<H265EncSlice>* slices,
                                     EncStatistics* stats) {
  if (!core_) return H265EncResult::NoMemory;
  if (!out || !slices || !H265PictureEncoder::sequence_valid(pic.seq)) return H265EncResult::PictureControlNotSupported;
  if (src.width != pic.seq.width || src.height != pic.seq.height || !src.fits() || rec.width != pic.seq.width || rec.height != pic.seq.height || !rec.fits())
    return H265EncResult::PictureControlNotSupported;
  const H265EncResult r = prepare(pic, src);
  if (r != H265EncResult::Ok) return r;
  // 1. decisions, coding tree block by coding tree block, slice by slice
  for (uint32_t s = 0; s < slice_count_; ++s) {
    slice_start_ = static_cast<int>(slice_bounds_[s]);
    qp_prev_ = pic.qp;
    for (uint32_t ctb = slice_bounds_[s]; ctb < slice_bounds_[s + 1]; ++ctb) {
      // the coding tree block's QP within reach of cu_qp_delta from the predicted one (7.4.9.14)
      const int lo = qp_prev_ - (26 + qp_bd_ / 2), hi = qp_prev_ + 25 + qp_bd_ / 2;
      const int target = clip3(imax(lo, -qp_bd_), imin(hi, 51), ctb_qp_[ctb]);
      ctb_qp_[ctb] = static_cast<int8_t>(target);
      ctu_qp_ = target;
      const int x = static_cast<int>(ctb % static_cast<uint32_t>(ctb_w_)) << log2_ctb_, y = static_cast<int>(ctb / static_cast<uint32_t>(ctb_w_)) << log2_ctb_;
      decide_cu(x, y, log2_ctb_, 0);
      const int n = 1 << log2_ctb_;
      bool any = false;
      for (int c = 0; c < 3 && !any; ++c) {
        const int f = c ? 2 : 1, pw = c ? cw_ : W_, ph = c ? ch_ : H_;
        for (int j = y / f; j < imin(ph, (y + n) / f) && !any; ++j)
          for (int i = x / f; i < imin(pw, (x + n) / f); ++i)
            if (coef_[c][static_cast<uint64_t>(j) * pw + i]) {
              any = true;
              break;
            }
      }
      if (any) qp_prev_ = target;
    }
  }
  // 2. and 3. the slices and the reconstruction
  drift_ = 0;
  if (check_drift) {
    pass_.clear();
    if (!write_slices(&pass_, &slices_tmp_, false, true)) return H265EncResult::NoMemory;
    if (!decode(pass_.view(), slices_tmp_.view(), true)) {
      drift_ = ~uint64_t{0};
    } else {
      for (int c = 0; c < 3; ++c)
        for (uint64_t i = 0; i < rec_[c].size(); ++i)
          if (picture_sample(target_, c, i) != rec_[c][i]) ++drift_;
    }
  }
  if (pic.seq.sao) {
    pass_.clear();
    if (!write_slices(&pass_, &slices_tmp_, false, false)) return H265EncResult::NoMemory;
    if (!decode(pass_.view(), slices_tmp_.view(), false)) return H265EncResult::NoMemory;
    choose_sao();
  }
  EncStatistics local;
  stats_ = &local;
  nal_.clear();
  const bool written = write_slices(&nal_, slices, pic.seq.sao, false);
  stats_ = nullptr;
  if (!written) return H265EncResult::NoMemory;
  if (!decode(nal_.view(), slices->view(), false)) return H265EncResult::NoMemory;
  const uint64_t base = out->size();
  if (!out->append(nal_.view())) return H265EncResult::NoMemory;
  for (H265EncSlice& s : slices->view()) s.offset += base;
  for (int c = 0; c < 3; ++c) {
    const ArrayView<uint16_t> d = c == 0 ? rec.y : c == 1 ? rec.u : rec.v;
    const uint64_t count = c == 0 ? static_cast<uint64_t>(W_) * H_ : static_cast<uint64_t>(cw_) * ch_;
    for (uint64_t i = 0; i < count; ++i) d[i] = static_cast<uint16_t>(picture_sample(target_, c, i));
  }
  if (stats) *stats = local;
  return H265EncResult::Ok;
}

}  // namespace h265

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------

H265PictureEncoder::H265PictureEncoder(allocators::IAllocator& allocator) : allocator_(allocator) {}
H265PictureEncoder::~H265PictureEncoder() {}

bool H265PictureEncoder::sequence_valid(const H265EncSequence& s) {
  if (s.bit_depth != 8 && s.bit_depth != 10) return false;
  if (s.log2_ctb < 4 || s.log2_ctb > 6 || s.log2_min_cb < 3 || s.log2_min_cb > s.log2_ctb) return false;
  if (s.log2_min_tb < 2 || s.log2_min_tb >= s.log2_min_cb || s.log2_max_tb < 3 || s.log2_max_tb > 5 || s.log2_max_tb > s.log2_ctb || s.log2_max_tb < s.log2_min_tb) return false;
  if (s.max_th_depth_inter > s.log2_ctb - s.log2_min_tb || s.max_th_depth_intra > s.log2_ctb - s.log2_min_tb) return false;
  if (s.log2_max_poc_lsb < 4 || s.log2_max_poc_lsb > 16) return false;
  const uint32_t min_cb = 1u << s.log2_min_cb;
  if (s.width < 16 || s.height < 16 || s.width > 8192 || s.height > 4320 || s.width % min_cb || s.height % min_cb) return false;
  const uint64_t ctb = uint64_t{1} << s.log2_ctb;
  return ((s.width + ctb - 1) / ctb) * ((s.height + ctb - 1) / ctb) <= static_cast<uint64_t>(h265::kMaxCtbs);
}

uint32_t H265PictureEncoder::slice_count(H265EncSliceMode mode, uint32_t value, uint32_t ctb_width, uint32_t ctb_height) {
  const uint64_t ctbs = uint64_t{ctb_width} * ctb_height;
  if (ctbs == 0) return 0;
  switch (mode) {
    case H265EncSliceMode::FullFrame: return 1;
    case H265EncSliceMode::CtbsPerSlice: return value == 0 ? 0 : static_cast<uint32_t>((ctbs + value - 1) / value);
    case H265EncSliceMode::RowsPerSlice: return value == 0 ? 0 : (ctb_height + value - 1) / value;
    case H265EncSliceMode::SlicesPerFrame: return value == 0 || value > ctbs ? 0 : value;
    default: return 0;   // (BytesPerSlice is not done)
  }
}

H265EncResult H265PictureEncoder::encode(const H265EncPicture& picture, const EncPlanes16& source, const EncWritablePlanes16& reconstruction, ByteBuffer* out,
                                         List<H265EncSlice>* slices, EncStatistics* statistics) {
  if (!core_) {
    core_ = UniquePointer<h265::PictureEncoder>::make(allocator_, allocator_);
    if (!core_) return H265EncResult::NoMemory;
  }
  core_->check_drift = check_drift_;
  const H265EncResult r = core_->encode(picture, source, reconstruction, out, slices, statistics);
  drift_ = core_->drift();
  return r;
}

bool H265PictureEncoder::write_parameter_sets(const H265EncSequence& seq, uint32_t visible_width, uint32_t visible_height, uint32_t pps_id, uint8_t level_idc,
                                              uint8_t max_sub_layers, ByteBuffer* out) {
  if (!sequence_valid(seq) || !out || pps_id > 63 || visible_width == 0 || visible_height == 0 || visible_width > seq.width || visible_height > seq.height ||
      (visible_width & 1) || (visible_height & 1) || max_sub_layers < 1 || max_sub_layers > 7)
    return false;
  const int subs = max_sub_layers - 1;
  auto ptl = [&](h265::BitWriter& w) {
    const int profile = seq.bit_depth == 8 ? 1 : 2;
    w.put(0, 2);         // general_profile_space
    w.bit(0);            // general_tier_flag
    w.put(static_cast<uint32_t>(profile), 5);
    for (int j = 0; j < 32; ++j) w.bit(j == profile || (profile == 1 && j == 2));   // general_profile_compatibility_flag
    w.bit(1);            // general_progressive_source_flag
    w.bit(0);            // general_interlaced_source_flag
    w.bit(0);            // general_non_packed_constraint_flag
    w.bit(1);            // general_frame_only_constraint_flag
    w.put(0, 32);        // general_reserved_zero_43bits and general_inbld_flag: 44 bits
    w.put(0, 12);
    w.put(level_idc, 8);
    for (int i = 0; i < subs; ++i) {
      w.bit(0);          // sub_layer_profile_present_flag
      w.bit(0);          // sub_layer_level_present_flag
    }
    if (subs > 0)
      for (int i = subs; i < 8; ++i) w.put(0, 2);
  };
  auto ordering = [&](h265::BitWriter& w) {
    w.bit(1);   // sub_layer_ordering_info_present_flag
    for (int i = 0; i <= subs; ++i) {
      w.ue(15);   // max_dec_pic_buffering_minus1
      w.ue(4);    // max_num_reorder_pics
      w.ue(0);    // max_latency_increase_plus1
    }
  };
  allocators::IAllocator& a = out->allocator();
  {
    h265::BitWriter w(a);
    w.put(0, 4);    // vps_video_parameter_set_id
    w.bit(1);       // vps_base_layer_internal_flag
    w.bit(1);       // vps_base_layer_available_flag
    w.put(0, 6);    // vps_max_layers_minus1
    w.put(static_cast<uint32_t>(subs), 3);
    w.bit(subs == 0);
    w.put(0xFFFF, 16);
    ptl(w);
    ordering(w);
    w.put(0, 6);    // vps_max_layer_id
    w.ue(0);        // vps_num_layer_sets_minus1
    w.bit(0);       // vps_timing_info_present_flag
    w.bit(0);       // vps_extension_flag
    w.trailing();
    if (!w.ok() || !h265::put_nal(out, h265::kVps, 0, w.bytes(), 0, nullptr)) return false;
  }
  {
    h265::BitWriter w(a);
    w.put(0, 4);
    w.put(static_cast<uint32_t>(subs), 3);
    w.bit(subs == 0);
    ptl(w);
    w.ue(0);   // sps_seq_parameter_set_id
    w.ue(1);   // chroma_format_idc
    w.ue(seq.width);
    w.ue(seq.height);
    const bool crop = visible_width != seq.width || visible_height != seq.height;
    w.bit(crop);
    if (crop) {
      w.ue(0);
      w.ue((seq.width - visible_width) / 2);
      w.ue(0);
      w.ue((seq.height - visible_height) / 2);
    }
    w.ue(seq.bit_depth - 8u);
    w.ue(seq.bit_depth - 8u);
    w.ue(seq.log2_max_poc_lsb - 4u);
    ordering(w);
    w.ue(seq.log2_min_cb - 3u);
    w.ue(static_cast<uint32_t>(seq.log2_ctb - seq.log2_min_cb));
    w.ue(seq.log2_min_tb - 2u);
    w.ue(static_cast<uint32_t>(seq.log2_max_tb - seq.log2_min_tb));
    w.ue(seq.max_th_depth_inter);
    w.ue(seq.max_th_depth_intra);
    w.bit(0);   // scaling_list_enabled_flag
    w.bit(seq.amp);
    w.bit(seq.sao);
    w.bit(0);   // pcm_enabled_flag
    w.ue(0);    // num_short_term_ref_pic_sets
    w.bit(seq.long_term_refs);
    if (seq.long_term_refs) w.ue(0);   // num_long_term_ref_pics_sps
    w.bit(0);   // sps_temporal_mvp_enabled_flag
    w.bit(0);   // strong_intra_smoothing_enabled_flag
    w.bit(0);   // vui_parameters_present_flag
    w.bit(0);   // sps_extension_present_flag
    w.trailing();
    if (!w.ok() || !h265::put_nal(out, h265::kSpsNal, 0, w.bytes(), 0, nullptr)) return false;
  }
  {
    h265::BitWriter w(a);
    w.ue(pps_id);
    w.ue(0);
    w.bit(0);       // dependent_slice_segments_enabled_flag
    w.bit(0);       // output_flag_present_flag
    w.put(0, 3);    // num_extra_slice_header_bits
    w.bit(0);       // sign_data_hiding_enabled_flag
    w.bit(0);       // cabac_init_present_flag
    w.ue(0);
    w.ue(0);
    w.se(0);        // init_qp_minus26
    w.bit(seq.constrained_intra_pred);
    w.bit(seq.transform_skip);
    w.bit(1);       // cu_qp_delta_enabled_flag
    w.ue(0);        // diff_cu_qp_delta_depth
    w.se(0);
    w.se(0);
    w.bit(0);       // pps_slice_chroma_qp_offsets_present_flag
    w.bit(0);       // weighted_pred_flag
    w.bit(0);       // weighted_bipred_flag
    w.bit(0);       // transquant_bypass_enabled_flag
    w.bit(0);       // tiles_enabled_flag
    w.bit(0);       // entropy_coding_sync_enabled_flag
    w.bit(seq.loop_filter_across_slices);
    w.bit(1);       // deblocking_filter_control_present_flag
    w.bit(0);       // deblocking_filter_override_enabled_flag
    w.bit(0);       // pps_deblocking_filter_disabled_flag
    w.se(0);
    w.se(0);
    w.bit(0);       // pps_scaling_list_data_present_flag
    w.bit(1);       // lists_modification_present_flag
    w.ue(0);        // log2_parallel_merge_level_minus2
    w.bit(0);       // slice_segment_header_extension_present_flag
    w.bit(0);       // pps_extension_present_flag
    w.trailing();
    if (!w.ok() || !h265::put_nal(out, h265::kPpsNal, 0, w.bytes(), 0, nullptr)) return false;
  }
  return true;
}

}  // namespace hardware
