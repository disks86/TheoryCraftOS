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


// The internals of the VC-1 decoder: headers, pictures and the Core class whose methods are spread over Vc1Syntax.cpp (headers, bit planes, picture management and
// output), Vc1Block.cpp (macroblocks, coefficients and motion vector prediction) and Vc1Recon.cpp (inverse transforms, motion compensation, overlap smoothing and
// the in-loop filter). The decoding process follows SMPTE 421M; where the standard leaves the order of operations to the implementation, the order is the one of the
// reference decoders, so the pictures are bit exact with theirs.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Hardware/VideoCodec/Software/H264Core.hpp"
#include "Hardware/VideoCodec/Software/Vc1.hpp"
#include "Hardware/VideoCodec/VideoTypes.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {
namespace vc1 {

using h264::Mem;

// An allocator-backed array whose element access is bounds checked.
template <class T>
class Buf {
 public:
  bool alloc(allocators::IAllocator& a, uint64_t count) { return m_.alloc(a, count); }
  void release() { m_.release(); }
  T* data() const { return m_.data(); }
  uint64_t size() const { return m_.size(); }
  ArrayView<T> view() const { return ArrayView<T>(m_.data(), m_.size()); }
  T& operator[](uint64_t i) const { return view()[i]; }
  void fill(T v) {
    for (T& x : view()) x = v;
  }

 private:
  Mem<T> m_;
};

constexpr int kPoolSize = 10;   // the two anchors, the picture being decoded, the pictures waiting for output, and the spare
constexpr int kOutputQueue = 6;
constexpr int kMaxMbWidth = 256, kMaxMbHeight = 256;   // 4096 x 4096

enum Profile : int { kSimple = 0, kMain = 1, kComplex = 2, kAdvanced = 3 };
enum PictureType : int { kPictureI = 0, kPictureP = 1, kPictureB = 2, kPictureBI = 3 };
enum MvMode : int { kMv1MvHpelBilin = 0, kMv1Mv = 1, kMv1MvHpel = 2, kMvMixed = 3, kMvIntensityComp = 4 };
enum TransformType : int { kTt8x8 = 0, kTt8x4Bottom, kTt8x4Top, kTt8x4, kTt4x8Right, kTt4x8Left, kTt4x8, kTt4x4 };
enum QuantMode : int { kQuantFrameImplicit = 0, kQuantFrameExplicit, kQuantNonUniform, kQuantUniform };
enum DqProfile : int { kDqFourEdges = 0, kDqDoubleEdges, kDqSingleEdge, kDqAllMbs };
enum BmvType : int { kBmvBackward = 0, kBmvForward, kBmvInterpolated, kBmvDirect };
enum CondOver : int { kCondOverNone = 0, kCondOverAll, kCondOverSelect };
enum CodingSet : int { kCsHighMotIntra = 0, kCsHighMotInter, kCsLowMotIntra, kCsLowMotInter, kCsMidRateIntra, kCsMidRateInter, kCsHighRateIntra, kCsHighRateInter };

// 6.1 and 6.2: the sequence layer (and, for the Advanced profile, the entry point layer, whose fields replace the sequence's).
struct SequenceHeader {
  bool valid = false;
  int profile = kSimple;
  int level = 0;
  int frmrtq_postproc = 0, bitrtq_postproc = 0;
  bool loop_filter = false, multires = false, fastuvmc = false, extended_mv = false, vstransform = false, overlap = false, resync_marker = false, rangered = false;
  int dquant = 0, max_b_frames = 0, quantizer_mode = 0;
  bool finterpflag = false;
  bool res_x8 = false, res_rtm_flag = true;
  // Advanced profile
  bool postprocflag = false, broadcast = false, interlace = false, tfcntrflag = false, psf = false;
  bool panscanflag = false, refdist_flag = false, extended_dmv = false, hrd_param_flag = false;
  int hrd_num_leaky_buckets = 0;
  int max_coded_width = 0, max_coded_height = 0;
  bool range_mapy_flag = false, range_mapuv_flag = false;
  // The size of the pictures (the container's for Simple and Main, the entry point's or the sequence's for Advanced) and what follows from it.
  int width = 0, height = 0, mb_width = 0, mb_height = 0;
};

// 7: the picture layer, for progressive pictures.
struct PictureHeader {
  int type = kPictureI;
  bool skipped = false;      // a P picture that repeats the one before
  bool rangeredfrm = false;
  bool interpfrm = false;
  int bfraction_index = 0, bfraction = 0;
  int rnd = 0;
  int pqindex = 0, pq = 0;
  bool halfpq = false, pquantizer = false;
  int mvrange = 0, k_x = 0, k_y = 0, range_x = 0, range_y = 0;
  int respic = 0;
  int mv_mode = kMv1Mv, mv_mode2 = kMv1Mv;
  int lumscale = 32, lumshift = 0;
  bool use_ic = false;       // intensity compensation of the reference picture
  bool quarter_sample = true, mspel = true;
  int tt_index = 0;
  bool ttmbf = true;
  int ttfrm = kTt8x8;
  bool dquantfrm = false;
  int dqprofile = 0, dqsbedge = 0;
  bool dqbilevel = false;
  int altpq = 0;
  int c_ac_table_index = 0, y_ac_table_index = 0, dc_table_index = 0;
  int cbptab = 0, mv_table_index = 0;
  int condover = kCondOverNone;
  int postproc = 0;
  // The bit planes (stored in the Core), and whether each is raw: coded bit by bit in the macroblock layer.
  bool mv_type_raw = false, skip_raw = false, direct_raw = false, acpred_raw = false, overflags_raw = false;
};

// A decoded frame.
struct Picture {
  Mem<uint8_t> plane[3];
  int stride[3] = {0, 0, 0};
  int width = 0, height = 0;     // luma, whole macroblocks
  int64_t pts = 0;
  int type = kPictureI;
  bool in_use = false;           // not available for a new picture
  bool output_pending = false;   // in the output queue
  Mem<int16_t> direct_mv;        // per macroblock: the motion vector direct mode scales (the one of the first luma block; zero for intra)
};

// What an offload (a hardware decoder) is given for each picture: the headers, the pictures it refers to (by slot: the index in the Core's pool) and the picture's
// data: the frame unit and its slice units, as they are in the stream (Advanced profile: each behind its start code; Simple and Main: the frame as stored).
// Where the macroblocks of a unit of a picture begin (the frame unit, then each slice unit): the first macroblock row, and the bit position of the first macroblock in the unit's
// bits with the emulation prevention bytes removed, counted from the first bit after the start code (Simple and Main profile: from the first bit of the frame).
struct UnitInfo {
  int start_row = 0;
  uint32_t mb_bit_offset = 0;
};

struct OffloadPicture {
  const SequenceHeader* seq = nullptr;
  const PictureHeader* pic = nullptr;
  int slot = 0;
  int forward_slot = -1, backward_slot = -1;   // the pictures this one predicts from (-1: none)
  int anchor_slot[2] = {-1, -1};               // the older and the newer anchor picture when this one starts: the pictures an offload must still hold
  ArrayView<const ConstByteView> units;
  ArrayView<const UnitInfo> unit_info;         // one per unit
  // The bit planes the picture codes (one byte per macroblock, 0 or 1, in raster order); empty for a plane that is not there or is coded in the macroblock layer ("raw").
  ArrayView<const uint8_t> skip_plane, mv_type_plane, direct_plane, acpred_plane, overflags_plane;
  // Intensity compensation of the forward reference (set by the P picture that follows it, and in force for the B pictures between): the 6 bit scale and shift of the header.
  bool ic_forward = false;
  int ic_scale = 0, ic_shift = 0;
  // The frame to fill (8-bit planes).
  ByteView plane[3];
  int stride[3] = {0, 0, 0};
};
class IOffload {
 public:
  // Whether the stream can be decoded by the offload; asked at every sequence header (and so at the start of every picture group). A stream it refuses is decoded here.
  virtual bool accepts(const SequenceHeader& seq) = 0;
  // Decodes the picture into the planes. False: the picture's lines are left grey.
  virtual bool decode_picture(const OffloadPicture& picture) = 0;

 protected:
  constexpr IOffload() = default;
  ~IOffload() = default;
};

// For diagnostics and tests: told of every picture the Core decodes itself, with the same description the offload gets and the decoded frame.
class IFrameObserver {
 public:
  virtual void picture_decoded(const OffloadPicture& picture) = 0;

 protected:
  constexpr IFrameObserver() = default;
  ~IFrameObserver() = default;
};

// The bits of a (de-escaped) unit, most significant first. Reading past the end gives zeros and sets error().
class BitReader {
 public:
  BitReader() = default;
  explicit BitReader(ConstByteView data) : data_(data) {}
  uint32_t peek(int n) const {   // n <= 32
    if (n <= 0) return 0;
    uint64_t v = 0;
    const uint64_t byte = pos_ >> 3;
    for (uint64_t i = 0; i < 5; ++i) v = (v << 8) | (byte + i < data_.size() ? data_[byte + i] : 0);
    return static_cast<uint32_t>(((v << (pos_ & 7)) & 0xFFFFFFFFFFull) >> (40 - n));
  }
  uint32_t get(int n) {
    const uint32_t v = peek(n);
    skip(n);
    return v;
  }
  int bit() { return static_cast<int>(get(1)); }
  void skip(int n) {
    pos_ += static_cast<uint64_t>(n);
    if (pos_ > data_.size() * 8) error_ = true;
  }
  uint64_t bits_left() const { return pos_ >= data_.size() * 8 ? 0 : data_.size() * 8 - pos_; }
  bool overrun() const { return pos_ > data_.size() * 8; }
  bool error() const { return error_; }
  uint64_t position() const { return pos_; }
  // The unary code of ffmpeg's get_unary: counts the bits that differ from `stop`, up to `len`.
  int unary(int stop, int len) {
    int i = 0;
    while (i < len && bit() != stop) ++i;
    return i;
  }
  // decode012: 0, 10 or 11 as 0, 1, 2.
  int decode012() { return bit() ? bit() + 1 : 0; }

 private:
  ConstByteView data_;
  uint64_t pos_ = 0;
  bool error_ = false;
};

// A variable length code: a lookup table for the short codes and a search for the long ones.
class Vlc {
 public:
  template <class C>
  bool build(allocators::IAllocator& a, const C* codes, const uint8_t* bits, int count, int lut_bits) {
    lut_bits_ = lut_bits;
    count_ = count;
    if (!lut_.alloc(a, uint64_t{1} << lut_bits) || !codes_.alloc(a, static_cast<uint64_t>(count)) || !bits_.alloc(a, static_cast<uint64_t>(count))) return false;
    for (int sym = 0; sym < count; ++sym) {
      codes_[static_cast<uint64_t>(sym)] = static_cast<uint32_t>(codes[sym]);
      bits_[static_cast<uint64_t>(sym)] = bits[sym];
      const int len = bits[sym];
      if (len == 0 || len > lut_bits) continue;
      const uint32_t first = static_cast<uint32_t>(codes[sym]) << (lut_bits - len);
      for (uint32_t i = 0; i < (1u << (lut_bits - len)); ++i) lut_[first + i] = static_cast<uint16_t>((len << 9) | (sym + 1));
    }
    return true;
  }
  // The symbol, or -1 when the bits are no code.
  int decode(BitReader& br) const {
    const uint16_t entry = lut_[br.peek(lut_bits_)];
    if (entry != 0) {
      br.skip(entry >> 9);
      return (entry & 511) - 1;
    }
    const uint32_t window = br.peek(32);
    for (int sym = 0; sym < count_; ++sym) {
      const int len = bits_[static_cast<uint64_t>(sym)];
      if (len <= lut_bits_ || len > 32) continue;
      if ((window >> (32 - len)) == codes_[static_cast<uint64_t>(sym)]) {
        br.skip(len);
        return sym;
      }
    }
    return -1;
  }

 private:
  Buf<uint16_t> lut_;
  Buf<uint32_t> codes_;
  Buf<uint8_t> bits_;
  int lut_bits_ = 0, count_ = 0;
};

// The inverse transforms (clause 8.1.2): the 8x8 one in place, the others add their result to the samples at `offset` of `plane`.
void inv_trans_8x8(int16_t* block);
void inv_trans_8x4_add(ByteView plane, uint64_t offset, int stride, int16_t* block);
void inv_trans_4x8_add(ByteView plane, uint64_t offset, int stride, int16_t* block);
void inv_trans_4x4_add(ByteView plane, uint64_t offset, int stride, int16_t* block);
void add_block_clamped(ByteView plane, uint64_t offset, int stride, const int16_t* block);
// Writes the 8x8 samples of `block` (plus 128 when `signed_put`), clipped to 0..255.
void put_block_clamped(ByteView plane, uint64_t offset, int stride, const int16_t* block, bool signed_put);

class Core {
 public:
  explicit Core(allocators::IAllocator& allocator);
  ~Core() = default;
  Core(const Core&) = delete;
  Core& operator=(const Core&) = delete;

  bool configure(ConstByteView extradata, uint32_t width, uint32_t height);
  bool decode(ConstByteView data, int64_t pts_us);
  bool receive(VideoFrame* frame);
  void flush();
  void reset();
  VideoFormat output_format() const;
  bool ready() const { return queue_count_ > 0; }
  void set_offload(IOffload* offload) {
    offload_ = offload;
    offloaded_ = false;
  }
  void set_frame_observer(IFrameObserver* observer) { observer_ = observer; }

 private:
  // --- Vc1Syntax.cpp
  bool build_vlcs();
  bool parse_sequence_sm(BitReader& br);
  bool parse_sequence_adv(BitReader& br);
  bool parse_entry_point(BitReader& br);
  bool parse_picture_header(BitReader& br);
  bool decode_bitplane(BitReader& br, Buf<uint8_t>* plane, bool* raw);
  bool decode_dquant(BitReader& br);
  bool setup_sequence();
  bool decode_frame(ConstByteView frame, ArrayView<const ConstByteView> slices, ArrayView<const ConstByteView> raw_units);
  bool start_picture();
  void finish_picture(ArrayView<const ConstByteView> units);
  void output_picture(Picture* p);
  Picture* find_free_picture();
  // --- Vc1Block.cpp
  bool decode_slice_blocks(BitReader& br, int start_mb_y, int end_mb_y);
  void select_coding_sets(bool intra_picture);
  bool decode_i_mb(BitReader& br);
  bool decode_i_mb_adv(BitReader& br);
  bool decode_p_mb(BitReader& br);
  bool decode_b_mb(BitReader& br);
  bool decode_mquant(BitReader& br, int* mquant);
  bool decode_ac_coeff(BitReader& br, int* last, int* skip, int* value, int codingset);
  bool decode_i_block(BitReader& br, int16_t* block, int n, bool coded, int codingset);
  bool decode_i_block_adv(BitReader& br, int16_t* block, int n, bool coded, int codingset, int mquant);
  bool decode_intra_block(BitReader& br, int16_t* block, int n, bool coded, int mquant, int codingset);
  int decode_p_block(BitReader& br, int16_t* block, int n, int mquant, int ttmb, bool first_block, ByteView plane, uint64_t offset, int stride, uint32_t* ttmb_out);
  bool read_mvdata(BitReader& br, int* dmv_x, int* dmv_y, bool* has_coeffs, bool* intra);
  int pred_dc(int n, int quant, bool a_avail, bool c_avail, int16_t** dc_val, int* dir);
  int pred_dc_i(int n, int16_t** dc_val, int* dir);
  int coded_block_pred(int n, int diff);
  void pred_mv(BitReader& br, int n, int dmv_x, int dmv_y, bool mv1, int r_x, int r_y);
  void pred_b_mv(int dmv_x[2], int dmv_y[2], bool direct, int mvtype);
  void block_target(int n, int* plane, uint64_t* offset, int* stride) const;
  void keep_intra_block(int n, const int16_t* block);
  // --- Vc1Recon.cpp
  const Picture* reference_for(int dir) const;
  // Intensity compensation of the forward reference: a P picture's own, kept for the B pictures that follow it (they predict from the compensated reference too).
  bool ic_use_ = false;
  int ic_scale_ = 32, ic_shift_ = 0;
  void make_refs(const Picture* ref, bool use_ic, void* luma, void* cb, void* cr) const;
  void predict_mb(const Picture* ref, bool use_ic, int mx, int my, int uvmx, int uvmy, bool avg);
  void mc_1mv(int dir);
  void mc_4mv_luma(int n);
  void mc_4mv_chroma();
  void interp_mc();
  void b_mc(bool direct, int mode);
  int16_t* mb_block(int mx, int my, int n) const;
  void finish_intra_and_filters();
  void overlap_filter_i();
  void overlap_filter_p();
  void put_intra_blocks(bool signed_put);
  void loop_filter_i();
  void loop_filter_p();

  // --- state
  allocators::IAllocator& allocator_;
  SequenceHeader seq_;
  PictureHeader pic_;
  bool have_seq_ = false, sequence_ok_ = false;
  int64_t pts_ = 0;

  Picture pool_[kPoolSize];
  Picture* cur_ = nullptr;
  Picture* anchor_[2] = {nullptr, nullptr};   // older and newer I or P picture
  Picture* held_ = nullptr;                   // an anchor waiting for the next anchor before it is output
  Picture* queue_[kOutputQueue] = {};
  int queue_count_ = 0;
  Picture* returned_ = nullptr;
  bool need_intra_ = true;
  bool low_delay_ = true;
  int rnd_ = 0;                               // the rounding control of Simple and Main profile (toggles at each P picture)

  IOffload* offload_ = nullptr;
  IFrameObserver* observer_ = nullptr;
  bool offloaded_ = false;
  VideoFormat out_format_;

  // tables
  bool vlcs_ready_ = false;
  Vlc vlc_imode_, vlc_norm2_, vlc_norm6_, vlc_intra_cbpcy_, vlc_ac_[8], vlc_dc_[2][2], vlc_cbpcy_[4], vlc_ttmb_[3], vlc_ttblk_[3], vlc_subblkpat_[3], vlc_mv_diff_[4];
  uint8_t zz_8x8_[4][64];
  const uint8_t* zz_8x4_ = nullptr;
  const uint8_t* zz_4x8_ = nullptr;

  // bit planes of the picture being decoded (one byte per macroblock)
  Buf<uint8_t> plane_skip_, plane_mvtype_, plane_direct_, plane_acpred_, plane_overflags_;
  List<UnitInfo> unit_infos_;

  // picture decoding state
  int mb_w_ = 0, mb_h_ = 0;
  int mb_x_ = 0, mb_y_ = 0;
  int start_mb_y_ = 0, end_mb_y_ = 0;
  bool first_slice_line_ = true;
  bool mb_intra_ = false;
  int esc3_level_length_ = 0, esc3_run_length_ = 0;
  int codingset_ = 0, codingset2_ = 0;
  int y_dc_scale_ = 0;
  int a_avail_ = 0, c_avail_ = 0;
  bool ac_pred_ = false;
  int ref_w_ = 0, ref_h_ = 0;                  // the size motion vectors are clamped to
  Buf<uint8_t> slice_start_row_;                // per macroblock row: the row starts a slice
  Buf<int8_t> qscale_;                          // per macroblock: the quantizer, negative for "no half step"
  Buf<uint8_t> intra_mask_;                     // per macroblock: bit i set when block i is intra
  Buf<uint32_t> cbp_blocks_, ttblk_;            // per macroblock: the coded sub-blocks of each block (4 bits each), the transform types
  Buf<int16_t> mv_;                             // per 8x8 luma block (2 mb_w by 2 mb_h), x and y: the vector of the forward direction
  Buf<int16_t> mv_back_;                        // the same for the backward direction (B pictures)
  Buf<int16_t> uvmv_;                           // per macroblock: the chroma vector
  Buf<int16_t> dc_[3];                          // the quantized DC values of the blocks (luma 2 mb_w by 2 mb_h, chroma mb_w by mb_h)
  Buf<int16_t> ac_[3];                          // 16 values per block: the AC prediction values
  Buf<uint8_t> coded_;                          // luma blocks: the coded block flags, for prediction
  Buf<int16_t> blocks_;                         // per macroblock: the six blocks of the intra samples, waiting for the overlap filter
  int16_t cur_blk_[6][64];                      // the blocks of the macroblock being decoded
  int16_t mv_cur_[2][4][2];                     // the vectors of the macroblock being decoded: [direction][block][x / y]
};

}  // namespace vc1
}  // namespace hardware
