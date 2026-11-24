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
// The frame coding mode of an Advanced profile frame in an interlaced sequence (the FCM syntax element).
enum FrameCodingMode : int { kFcmProgressive = 0, kFcmFrameInterlace = 1, kFcmFieldInterlace = 2 };
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

// 7 and 9: the picture layer. A field interlace frame (two field pictures) has one of these per field while it is decoded: the second field's starts as a copy of
// the frame's part (FCM, FPTYPE, TFF, RNDCTRL, UVSAMP, REFDIST, BFRACTION) and has its own from PQINDEX on.
struct PictureHeader {
  // Interlaced pictures that an engine (or a DXVA driver) decodes are read only as far as UVSAMP and the engine takes the rest: `type` is then the frame's class
  // (an I or P frame has an intra first field; a B frame has B or BI fields only) and none of the fields below RNDCTRL are filled in.
  int fcm = kFcmProgressive;
  int fptype = 0;            // the field picture type (field interlace): first field class in bits 2:1, second in bits 2 and 0
  bool tff = true, rff = false;
  int rptfrm = 0;
  bool uvsamp = false;
  int type = kPictureI;
  bool skipped = false;      // a P picture that repeats the one before
  bool rangeredfrm = false;
  bool interpfrm = false;
  int bfraction_index = 0, bfraction = 0;
  int refdist = 0, frfd = 0, brfd = 0;   // field pictures: the reference distance (REFDIST), and for B fields the forward and backward distances from it
  int rnd = 0;
  int pqindex = 0, pq = 0;
  bool halfpq = false, pquantizer = false;
  int mvrange = 0, dmvrange = 0, k_x = 0, k_y = 0, range_x = 0, range_y = 0;
  int respic = 0;
  int mv_mode = kMv1Mv, mv_mode2 = kMv1Mv;
  int lumscale = 32, lumshift = 0;
  int lumscale2 = 32, lumshift2 = 0;   // field pictures: the bottom reference field's (the first pair is the top one's)
  int intcompfield = 0;                // field pictures: which reference fields are compensated (bit 0 top, bit 1 bottom)
  bool use_ic = false;       // intensity compensation of the reference picture
  bool quarter_sample = true, mspel = true;
  int numref = 0, reffield = 0;        // P fields: two reference fields, else which one (0 the closest)
  int mbmodetab = 0, imvtab = 0, icbptab = 0, fourmvbptab = 0;   // interlaced pictures: the macroblock mode, motion vector, coded block and 4MV block pattern tables
  int twomvbptab = 0;                  // interlaced frame pictures: the 2MV block pattern table
  bool fourmvswitch = false;           // interlaced frame P pictures: macroblocks can have four vectors (4MVSWITCH)
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
  bool mv_type_raw = false, skip_raw = false, direct_raw = false, acpred_raw = false, overflags_raw = false, forward_raw = false, fieldtx_raw = false;
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
  Mem<int16_t> direct_mv;        // per macroblock: the motion vector direct mode scales (the one of the first luma block; zero for intra). Two field pictures: the first
                                 // field's macroblocks, then the second's (by decoding order)
  Mem<uint8_t> mb_flags;         // field pictures, per macroblock as direct_mv: bit 0 intra, bits 1 to 4 the vector of luma block 0 to 3 refers to the opposite field
  Mem<int16_t> direct_mv_bottom; // interlaced frame pictures, per macroblock: the vector of luma block 2 (direct_mv has block 0's), for the field vectors of direct mode
  bool interlaced = false;       // coded as fields or as an interlaced frame: a reference read field by field
  bool field_coded = false;      // two field pictures
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

  // Decoding for a DXVA driver (src/Hardware/VideoCodec/Dxva/DxvaVc1.cpp): the caller gives the sequence (and entry point) layer as a SequenceHeader, keeps the
  // pictures and says which ones a picture is decoded into and predicted from; the picture is decoded at once (decoding order) and nothing goes to the output
  // queue. A Core used this way is used only this way.
  struct ExternalPicture {
    Picture* target = nullptr;     // the picture to fill (allocated here at the sequence's size when it is not)
    Picture* forward = nullptr;    // P: the anchor it predicts from (a skipped P picture: the one it repeats); B: the older anchor
    Picture* backward = nullptr;   // B: the newer anchor (its direct mode vectors are used too). Every reference the type needs must be given.
    int rnd = -1;                  // Simple and Main profile: the picture's rounding control (DXVA's bRcontrol); -1: the decoder's own history
    // B pictures: the intensity compensation of the forward reference, which the P picture of the backward anchor set.
    bool ic_forward = false;
    int ic_scale = 32, ic_shift = 0;
  };
  enum class ExternalStatus : uint8_t { Decoded, BadSequence, BadPicture, Interlaced, NoMemory };
  // `data`: one frame, as decode() takes it (Advanced profile: the frame unit and its slice units with their start codes, or the frame without a start code; a
  // sequence header or entry point unit in front is skipped: `seq` is what is in force). `header` receives the picture header the frame has (its type, whether it
  // is skipped, its intensity compensation) also when the picture is refused after the header was read.
  [[nodiscard]] ExternalStatus decode_external(const SequenceHeader& seq, ConstByteView data, const ExternalPicture& picture, PictureHeader* header);

  // A unit of a frame as decode() found it: its start code (0x0D for the frame itself, also when it has none), the bits with the emulation prevention bytes removed,
  // and the unit as it is in the stream.
  struct Part {
    uint32_t code = 0;
    ConstByteView data, raw;
  };

 private:
  // Where a picture header is read from: the frame's, a slice's (SLICE_ADDR then PIC_HEADER_FLAG set: the whole picture header again), the second field's (after
  // the field start code: it starts at PQINDEX).
  enum class HeaderAt : uint8_t { Frame, Slice, SecondField };
  // Intensity compensation tables (the model of the reference decoder, which the reference pictures follow): for each reference field (or line parity of a frame)
  // a luma and a chroma table, and whether they are in force. Updates chain onto what is there.
  struct IcLuts {
    uint8_t y[2][256];
    uint8_t uv[2][256];
    bool use = false;
  };
  // A reference for motion compensation: the picture, which of its fields (-1: the frame) and the tables its samples go through (null: none).
  struct RefSel {
    const Picture* pic = nullptr;
    int field = -1;
    const IcLuts* ic = nullptr;
  };

  // --- Vc1Syntax.cpp
  bool build_vlcs();
  bool parse_sequence_sm(BitReader& br);
  bool parse_sequence_adv(BitReader& br);
  bool parse_entry_point(BitReader& br);
  [[nodiscard]] bool parse_picture_header(BitReader& br, HeaderAt at);
  [[nodiscard]] bool decode_bitplane(BitReader& br, Buf<uint8_t>* plane, bool* raw);
  [[nodiscard]] bool decode_dquant(BitReader& br);
  bool setup_sequence();
  bool decode_frame(ArrayView<const Part> parts, ArrayView<const ConstByteView> raw_units);
  [[nodiscard]] bool walk_progressive_slices(BitReader& br, ArrayView<const Part> parts);
  void decode_field_pair(BitReader& br, ArrayView<const Part> parts);
  void begin_field(bool second);
  void finish_field();
  bool start_picture();
  void finish_picture(ArrayView<const ConstByteView> units);
  void output_picture(Picture* p);
  Picture* find_free_picture();
  void reset_picture_state();
  // intensity compensation
  void rotate_luts(int type);
  static void init_lut(int lumscale, int lumshift, IcLuts& luts, int field, bool chain);
  IcLuts& ic_curr() { return ic_curr_is_aux_ ? ic_aux_ : ic_next_; }
  // --- Vc1Block.cpp
  bool decode_slice_blocks(BitReader& br, int start_mb_y, int end_mb_y);
  void select_coding_sets(bool intra_picture);
  bool decode_i_mb(BitReader& br);
  bool decode_i_mb_adv(BitReader& br);
  bool decode_p_mb(BitReader& br);
  bool decode_b_mb(BitReader& br);
  bool decode_p_mb_field(BitReader& br);
  bool decode_b_mb_field(BitReader& br);
  bool decode_field_intra_mb(BitReader& br, int idx_mbmode, bool put_now);
  bool decode_p_mb_frame(BitReader& br);
  bool decode_b_mb_frame(BitReader& br);
  bool decode_frame_intra_mb(BitReader& br, bool put_now);
  void pred_mv_frame(int n, int dmv_x, int dmv_y, int mvn, int dir);
  bool decode_field_residual(BitReader& br, int cbp, int mquant, uint32_t* block_cbp, uint32_t* block_tt);
  bool decode_mquant(BitReader& br, int* mquant);
  bool decode_ac_coeff(BitReader& br, int* last, int* skip, int* value, int codingset);
  bool decode_i_block(BitReader& br, int16_t* block, int n, bool coded, int codingset);
  bool decode_i_block_adv(BitReader& br, int16_t* block, int n, bool coded, int codingset, int mquant);
  bool decode_intra_block(BitReader& br, int16_t* block, int n, bool coded, int mquant, int codingset);
  int decode_p_block(BitReader& br, int16_t* block, int n, int mquant, int ttmb, bool first_block, ByteView plane, uint64_t offset, int stride, uint32_t* ttmb_out);
  bool read_mvdata(BitReader& br, int* dmv_x, int* dmv_y, bool* has_coeffs, bool* intra);
  bool read_mvdata_interlaced(BitReader& br, int* dmv_x, int* dmv_y, int* pred_flag);
  int pred_dc(int n, int quant, bool a_avail, bool c_avail, int16_t** dc_val, int* dir);
  int pred_dc_i(int n, int16_t** dc_val, int* dir);
  int coded_block_pred(int n, int diff);
  void pred_mv(BitReader& br, int n, int dmv_x, int dmv_y, bool mv1, int r_x, int r_y);
  void pred_mv_field(BitReader& br, int n, int dmv_x, int dmv_y, bool mv1, int pred_flag, int dir);
  int scale_for_same(int value, int dim, int dir) const;
  int scale_for_opp(int value, int dim, int dir) const;
  void pred_b_mv(int dmv_x[2], int dmv_y[2], bool direct, int mvtype);
  [[nodiscard]] bool pred_b_mv_field(int n, int dmv_x[2], int dmv_y[2], bool mv1, int pred_flag[2]);
  void block_target(int n, int* plane, uint64_t* offset, int* stride) const;
  void keep_intra_block(int n, const int16_t* block);
  uint64_t field_mb_index(uint64_t mb_pos) const { return (second_field_ ? static_cast<uint64_t>(mb_w_) * static_cast<uint64_t>(mb_rows_) : 0) + mb_pos; }
  // --- Vc1Recon.cpp
  RefSel reference_for(int dir, int ref_field) const;
  void predict_mb(const RefSel& ref, int mx, int my, int uvmx, int uvmy, bool avg);
  void mc_1mv(int dir);
  void mc_4mv_luma(int n, int dir);
  void mc_4mv_chroma(int dir);
  void mc_4mv_luma_frame(int n, int dir, bool avg);
  void mc_4mv_chroma_frame(int dir, int dir2, bool avg);
  void store_frame_direct_mv(int n, int mx, int my);
  void interp_mc();
  void b_mc(bool direct, int mode);
  int16_t* mb_block(int mx, int my, int n) const;
  void finish_intra_and_filters();
  void overlap_filter_i();
  void overlap_filter_p();
  void put_intra_blocks(bool signed_put);
  void loop_filter_i();
  void loop_filter_p();
  void loop_filter_b_field();
  void overlap_filter_frame(bool intra_picture);
  void loop_filter_i_frame();
  void loop_filter_inter_frame();
  bool frame_interlace() const { return pic_.fcm == kFcmFrameInterlace; }
  bool mb_fieldtx(int mx, int my) const;
  ByteView dst_plane(int c) const { return ByteView(cur_->plane[c].data(), cur_->plane[c].size()); }

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

  // Intensity compensation as an offload is told of it: a P picture's own, kept for the B pictures that follow it (they predict from the compensated reference too).
  bool ic_use_ = false;
  int ic_scale_ = 32, ic_shift_ = 0;
  // ... and the tables the CPU decoder reads its references through: `last` for the forward reference, `next` for the backward one (and, in a P frame, for the
  // current frame's first field as its second field sees it), `aux` for the first field of a B frame as its second field sees it.
  IcLuts ic_last_, ic_next_, ic_aux_;
  bool ic_curr_is_aux_ = false;

  IOffload* offload_ = nullptr;
  IFrameObserver* observer_ = nullptr;
  const ExternalPicture* ext_ = nullptr;      // decode_external's picture while it is decoded
  bool offloaded_ = false;
  VideoFormat out_format_;

  // tables
  bool vlcs_ready_ = false;
  Vlc vlc_imode_, vlc_norm2_, vlc_norm6_, vlc_intra_cbpcy_, vlc_ac_[8], vlc_dc_[2][2], vlc_cbpcy_[4], vlc_ttmb_[3], vlc_ttblk_[3], vlc_subblkpat_[3], vlc_mv_diff_[4];
  // interlaced pictures (the frame picture ones too)
  Vlc vlc_if_mmv_mbmode_[8], vlc_if_1mv_mbmode_[8], vlc_mvdata1_[4], vlc_mvdata2_[8], vlc_icbpcy_[8], vlc_4mvbp_[4], vlc_2mvbp_[4], vlc_intfr_4mv_mbmode_[4],
      vlc_intfr_non4mv_mbmode_[4];
  uint8_t zz_8x8_[4][64];
  uint8_t zzi_8x8_[64];   // the 8x8 scan of interlaced pictures (transposed too)
  const uint8_t* zz_8x4_ = nullptr;
  const uint8_t* zz_4x8_ = nullptr;

  // bit planes of the picture being decoded (one byte per macroblock)
  Buf<uint8_t> plane_skip_, plane_mvtype_, plane_direct_, plane_acpred_, plane_overflags_, plane_forward_;
  Buf<uint8_t> plane_fieldtx_;   // interlaced frame pictures: the macroblock's luma blocks are fields (FIELDTX; P and B pictures: from the macroblock mode)
  List<UnitInfo> unit_infos_;

  // picture decoding state
  int mb_w_ = 0, mb_h_ = 0;
  int mb_rows_ = 0;                            // the macroblock rows of the picture being decoded (a field: half the frame's, rounded up)
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
  // Field pictures: the picture is a field, which one in decoding order, its parity (0 top, 1 bottom), the parity of the field each direction's vector refers to,
  // and the prediction type of the B macroblock being decoded.
  bool field_mode_ = false;
  bool second_field_ = false;
  int refdist_ = 0;                            // REFDIST of the last field pair that had it (B field pairs use it)
  int cur_field_ = 0;
  int ref_field_[2] = {0, 0};
  int bmvtype_ = kBmvBackward;
  // Where the picture being decoded starts in the planes of cur_ and the distance of its lines (a field: every other line of the frame).
  uint64_t dst_off_[3] = {0, 0, 0};
  int dst_stride_[3] = {0, 0, 0};
  Buf<uint8_t> slice_start_row_;                // per macroblock row: the row starts a slice
  Buf<int8_t> qscale_;                          // per macroblock: the quantizer, negative for "no half step"
  Buf<uint8_t> intra_mask_;                     // per macroblock: bit i set when block i is intra
  Buf<uint32_t> cbp_blocks_, ttblk_;            // per macroblock: the coded sub-blocks of each block (4 bits each), the transform types
  Buf<int16_t> mv_;                             // per 8x8 luma block (2 mb_w by 2 mb_h), x and y: the vector of the forward direction
  Buf<int16_t> mv_back_;                        // the same for the backward direction (B pictures)
  Buf<uint8_t> mv_f_[2];                        // field pictures, per 8x8 luma block and direction: the vector refers to the opposite field
  Buf<int16_t> uvmv_;                           // per macroblock: the chroma vector
  Buf<uint8_t> uvmv_f_;                         // field pictures, per macroblock: the forward chroma vector refers to the opposite field
  Buf<uint8_t> blk_mv_type_;                    // interlaced frame pictures, per 8x8 luma block: its vector is a field vector (2MV or 4MV field macroblocks)
  Buf<int16_t> dc_[3];                          // the quantized DC values of the blocks (luma 2 mb_w by 2 mb_h, chroma mb_w by mb_h)
  Buf<int16_t> ac_[3];                          // 16 values per block: the AC prediction values
  Buf<uint8_t> coded_;                          // luma blocks: the coded block flags, for prediction
  Buf<int16_t> blocks_;                         // per macroblock: the six blocks of the intra samples, waiting for the overlap filter
  int16_t cur_blk_[6][64];                      // the blocks of the macroblock being decoded
  int16_t mv_cur_[2][4][2];                     // the vectors of the macroblock being decoded: [direction][block][x / y]
};

}  // namespace vc1
}  // namespace hardware
