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

// The H.264 picture encoder's insides (H264PictureEncoder.hpp), shared by H264PictureEncoder.cpp (decisions, prediction, transforms, reconstruction, the in-loop filter) and
// H264PictureEncoderSyntax.cpp (the slice header, CAVLC and CABAC macroblock syntax). Clause numbers are ITU-T H.264's.
#pragma once
#include "Hardware/VideoCodec/Software/H264PictureEncoder.hpp"

namespace hardware {
namespace h264 {
namespace enc {

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
  struct Mark {
    uint64_t bytes = 0;
    uint8_t acc = 0;
    int count = 0;
    bool ok = true;
  };
  void reset() {
    buf_.clear();
    acc_ = 0;
    count_ = 0;
    ok_ = true;
  }
  Mark mark() const { return {buf_.size(), acc_, count_, ok_}; }
  void rewind(const Mark& m) {
    buf_.truncate(m.bytes);
    acc_ = m.acc;
    count_ = m.count;
    ok_ = m.ok;
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
  void align_with_ones() {
    while (count_ != 0) bit(1);
  }
  void align_with_zeros() {
    while (count_ != 0) bit(0);
  }
  uint64_t bits() const { return buf_.size() * 8 + static_cast<uint64_t>(count_); }
  int pending_bits() const { return count_; }
  void fail() { ok_ = false; }
  bool ok() const { return ok_; }
  ConstByteView bytes() const { return buf_.view(); }

 private:
  ByteBuffer buf_;
  uint8_t acc_ = 0;
  int count_ = 0;
  bool ok_ = true;
};

inline int ue_bits(uint32_t v) {
  int len = 0;
  while (((uint64_t{v} + 1) >> (len + 1)) != 0) ++len;
  return 2 * len + 1;
}
inline int se_bits(int v) { return ue_bits(v > 0 ? static_cast<uint32_t>(2 * v - 1) : static_cast<uint32_t>(-2 * v)); }

constexpr int kCabacContexts = 460;   // (frame coding, 4:2:0: ctxIdx 0 .. 459)

// 9.3.4.2: the arithmetic encoder, writing into a BitWriter. The whole state (contexts and engine) is a value, so that a slice can be taken back to a macroblock boundary.
class CabacWriter {
 public:
  struct State {
    uint8_t ctx[kCabacContexts] = {};   // (pStateIdx << 1) | valMPS
    uint32_t low = 0, range = 510;
    uint64_t outstanding = 0;
    bool first = true;
    uint64_t bins = 0;
  };
  // 9.3.1.1 / 9.3.1.2: contexts for the slice's QP and model (0 for I slices, cabac_init_idc + 1 otherwise), and the engine.
  void start(int model, int slice_qp);
  void decision(BitWriter& w, int ctx, int bin);
  void bypass(BitWriter& w, int bin);
  void terminate(BitWriter& w, int bin);   // a 1 also flushes (9.3.4.5: the last bit written is the rbsp_stop_one_bit)
  uint64_t bins() const { return s_.bins; }
  // The bits the engine holds that are not in the writer yet (a bound, for slice sizes).
  uint64_t pending_bits() const { return s_.outstanding + 10; }
  const State& state() const { return s_; }
  void restore(const State& s) { s_ = s; }

 private:
  void put_bit(BitWriter& w, int b);
  void renorm(BitWriter& w);
  State s_;
};

struct Vec {
  int x = 0, y = 0;
};
inline bool operator==(Vec a, Vec b) { return a.x == b.x && a.y == b.y; }
inline bool operator!=(Vec a, Vec b) { return !(a == b); }

// What the coded picture keeps of a macroblock (for the neighbours' predictions, the entropy coders' contexts and the in-loop filter).
struct MbData {
  int32_t slice = -1;
  bool intra = false, skip = false, direct = false, i16 = false, inxn = false, t8x8 = false;
  uint8_t qp = 0;
  uint8_t cbp = 0;                   // CodedBlockPatternLuma | CodedBlockPatternChroma << 4
  uint8_t chroma_mode = 0;           // intra_chroma_pred_mode
  uint8_t cbf_dc = 0;                // CABAC coded_block_flag of the DC blocks: bit 0 luma (Intra16x16), 1 Cb, 2 Cr
  int8_t ipred[16] = {};             // I_NxN: the prediction mode of each 4x4 block (raster; an 8x8 block's four the same)
  int8_t ref[2][4] = {{-1, -1, -1, -1}, {-1, -1, -1, -1}};   // reference index per list and 8x8 block (-1: the list is not used)
  int64_t ident[2][4] = {{-1, -1, -1, -1}, {-1, -1, -1, -1}};  // the identity of that reference picture
  int16_t mv[2][4][2] = {};
  int16_t mvd[2][2] = {};            // |mvd| per list and component (clipped to 127), for CABAC contexts
  uint8_t nz[24] = {};               // coefficients of each 4x4 block: 16 luma (raster), 4 Cb, 4 Cr (an 8x8 block's four: CAVLC their own counts, CABAC the block's)
  uint16_t nzmask = 0;               // luma 4x4 blocks with coefficients (an 8x8 transform block's four together), for the filter
};

// The direct prediction of a macroblock (8.4.1.2), 8x8 block by block.
struct DirectMotion {
  int8_t ref[2][4];
  Vec mv[2][4];
};

}  // namespace enc

using namespace enc;   // (only the encoder's own files include this header)

class PictureCoder {
 public:
  explicit PictureCoder(allocators::IAllocator& allocator) : bw_(allocator), mbs_(allocator), starts_(allocator) {}

  H264EncResult encode(const H264EncPicture& pic, const H264EncPlanes& src, const H264EncWritablePlanes& rec, ByteBuffer* out, List<H264EncSlice>* slices,
                       H264EncStatistics* stats, const H264EncOutputs& outputs);

 private:
  enum class Kind : uint8_t { I16, I8, Inter, Direct };
  // The macroblock being coded.
  struct Mb {
    Kind kind = Kind::I16;
    bool t8x8 = false;
    bool skip = false;
    int i16_mode = 2, chroma_mode = 0;
    int i8_mode[4] = {2, 2, 2, 2}, i8_pred[4] = {2, 2, 2, 2};
    int pred_flags = 1;                // Inter: bit 0 list 0, bit 1 list 1
    int ref[2] = {0, 0};
    Vec mv[2], mvp[2];
    int8_t ref8[2][4];                 // the motion of each 8x8 block (Inter: all four the partition's)
    Vec mv8[2][4];
    int qp = 26, qpc = 26;
    int cbp_luma = 0, cbp_chroma = 0;
    int dc[16];
    int ac[16][16];                    // 4x4 blocks' levels in scan order (Intra16x16: AC at 1 .. 15)
    int l8[4][64];                     // 8x8 blocks' levels in scan order
    int cdc[2][4];
    int cac[2][4][16];
    uint8_t pred_y[256];
    uint8_t pred_c[2][64];
    uint8_t nz[24];
    uint8_t cbf_dc = 0;
  };
  struct Neighbour {
    bool available = false;
    int ref = -1;
    Vec mv;
  };
  // A slice's coding state at a macroblock boundary (slices by bytes go back to one).
  struct Resume {
    BitWriter::Mark bits;
    CabacWriter::State cabac;
    uint32_t skip_run = 0;
    int qp_prev = 26;
    bool dqp_nonzero = false;
    H264EncStatistics stats;
    uint64_t epb_scanned = 0, epb_count = 0;
    int epb_zeros = 0;
  };

  bool p_slice() const { return pic_->type == H264EncFrameType::P; }
  bool b_slice() const { return pic_->type == H264EncFrameType::B; }
  bool cabac() const { return pic_->entropy == H264EncEntropy::Cabac; }
  uint64_t addr(int x, int y) const { return static_cast<uint64_t>(y) * mbw_ + static_cast<uint64_t>(x); }
  bool available(int x, int y) const { return x >= 0 && y >= 0 && x < static_cast<int>(mbw_) && y < static_cast<int>(mbh_) && mbs_[addr(x, y)].slice == slice_; }
  const MbData* mb_at(int x, int y) const { return available(x, y) ? &mbs_[addr(x, y)] : nullptr; }
  // (for intra prediction: constrained intra prediction leaves out inter macroblocks)
  bool intra_available(int x, int y) const { return available(x, y) && (!pic_->constrained_intra_pred || mbs_[addr(x, y)].intra); }

  // ---- H264PictureEncoder.cpp
  Neighbour neighbour(int x, int y, int b8, int list) const;
  Vec predict_mv(int mbx, int mby, int list, int ref) const;
  Vec skip_mv(int mbx, int mby) const;
  bool direct_motion(int mbx, int mby, DirectMotion* out) const;
  int predicted_i8_mode(int mbx, int mby, int b8, ArrayView<const int> cur_modes) const;

  bool pred_i16(int mode, int mbx, int mby, ByteView pred) const;
  bool pred_chroma(int mode, int mbx, int mby, int plane, ByteView pred) const;
  bool pred_i8(int mode, int mbx, int mby, int b8, ByteView pred) const;
  void pred_luma(int list, int ref, Vec mv, int x, int y, int w, int h, ByteView out) const;
  void pred_chroma_inter(int list, int ref, Vec mv, int x, int y, int w, int h, ByteView out_u, ByteView out_v) const;
  void build_inter_prediction(int mbx, int mby);
  uint32_t sad16(int mbx, int mby, ConstByteView pred) const;
  uint32_t satd16(int mbx, int mby, ConstByteView pred) const;
  int mv_cost(Vec mv, Vec pred) const { return lambda_ * (se_bits(mv.x - pred.x) + se_bits(mv.y - pred.y)); }
  int ref_cost(int list, int ref) const;
  bool usable_reference(int mby, int list, int ref) const;
  bool vector_allowed(int mbx, int mby, int list, int ref, Vec mv, int bx, int by, int w, int h) const;
  bool direct_allowed(int mbx, int mby, const DirectMotion& d) const;
  // The best vector into list[list][ref] (diamond search, then half and quarter refinement as the precision allows): cost (SAD + rate) and vector; false if no vector is
  // allowed.
  bool search(int mbx, int mby, int list, int ref, Vec pred, Vec* best, int* cost) const;

  void quant_luma_i16(int mbx, int mby);
  bool quant_luma_4x4(int mbx, int mby, uint64_t* cost);
  bool quant_luma_8x8(int mbx, int mby, bool intra, uint64_t* cost);
  void quant_chroma(int mbx, int mby, bool intra);
  void reconstruct_luma(int mbx, int mby);
  void reconstruct_chroma(int mbx, int mby);
  int code_i8(int mbx, int mby);   // Intra8x8: chooses each block's mode and codes it into the reconstruction; returns the cost
  void code_mb(int mbx, int mby);

  int boundary_strength(const MbData& p, const MbData& q, int blk_p, int blk_q, bool mb_edge) const;
  void deblock_mb(int mbx, int mby);

  Resume save() const;
  void restore(const Resume& r);
  uint64_t projected_slice_bytes();

  // ---- H264PictureEncoderSyntax.cpp
  void write_slice_header(uint32_t first_mb);
  void write_skip(int mbx, int mby);                                 // the macroblock is P_Skip / B_Skip
  void write_mb(int mbx, int mby, int qp_delta);       // CAVLC or CABAC, as the PPS says
  void write_mb_cavlc(int mbx, int mby, int qp_delta);
  void write_mb_cabac(int mbx, int mby, int qp_delta);
  void finish_slice_data();
  // CABAC syntax elements (9.3.3.1.1 context selection from the neighbours A and B)
  void cabac_mb_type(int mbx, int mby);
  void cabac_cbp(int mbx, int mby);
  void cabac_qp_delta(int delta);
  void cabac_ref_idx(int mbx, int mby, int list, int ref);
  void cabac_mvd(int mbx, int mby, int list, int comp, int value);
  bool cabac_cbf(int mbx, int mby, int cat, int bx, int by, int plane, bool coded);
  void cabac_residual(int cat, ArrayView<const int> levels, int max_coeff);

  const H264EncPicture* pic_ = nullptr;
  H264EncOutputs outputs_;
  H264EncPlanes src_;
  H264EncWritablePlanes rec_;
  BitWriter bw_;
  CabacWriter cw_;
  List<MbData> mbs_;
  List<uint32_t> starts_;
  uint32_t mbw_ = 0, mbh_ = 0;
  int32_t slice_ = 0;
  uint32_t slice_first_ = 0;
  int qp_prev_ = 26;
  bool dqp_nonzero_ = false;           // CABAC: the macroblock before had a non-zero mb_qp_delta
  int lambda_ = 1;
  uint32_t skip_run_ = 0;
  bool col_known_ = false;             // direct prediction can use RefPicList1[0]'s motion
  uint64_t epb_scanned_ = 0, epb_count_ = 0;
  int epb_zeros_ = 0;
  Mb m_;
  H264EncStatistics stats_;
};

}  // namespace h264
}  // namespace hardware
