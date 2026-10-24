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

// The internals of the H.264 decoder: parameter sets, pictures, the macroblock state and the Core class whose
// methods are spread over H264Syntax.cpp (NAL units, headers, reference pictures), H264Macroblock.cpp (slice data
// and macroblock parsing, CAVLC and CABAC), H264Predict.cpp (intra and inter prediction, transforms) and
// H264Deblock.cpp (the loop filter). Numbers in comments refer to ITU-T H.264.

#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Hardware/VideoCodec/Software/H264.hpp"
#include "Utility/AllocatedView.hpp"

namespace hardware {
namespace h264 {

constexpr int kMaxPictures = 18;     // 16 reference frames, the one being decoded and one waiting to be received
constexpr int kMaxMbs = 36864;       // 4096x2304
constexpr int kMaxRefs = 32;

// Memory from the allocator, zeroed, freed on destruction.
template <class T>
class Mem {
 public:
  Mem() = default;
  ~Mem() { release(); }
  Mem(const Mem&) = delete;
  Mem& operator=(const Mem&) = delete;
  bool alloc(allocators::IAllocator& a, uint64_t count) {
    if (a_ == &a && count_ == count && p_) {
      memset_zero();
      return true;
    }
    release();
    ArrayView<T> v = allocate_view<T>(a, count);
    if (v.empty()) return false;
    a_ = &a;
    p_ = v.data();
    count_ = count;
    memset_zero();
    return true;
  }
  void release() {
    if (p_) {
      ArrayView<T> v(p_, count_);
      release_view(*a_, v);
    }
    p_ = nullptr;
    count_ = 0;
  }
  T* data() const { return p_; }
  uint64_t size() const { return count_; }
  T& operator[](uint64_t i) const { return p_[i]; }

 private:
  void memset_zero() {
    uint8_t* b = reinterpret_cast<uint8_t*>(p_);
    for (uint64_t i = 0; i < count_ * sizeof(T); ++i) b[i] = 0;
  }
  allocators::IAllocator* a_ = nullptr;
  T* p_ = nullptr;
  uint64_t count_ = 0;
};

// 7.3 - reads the bits of a NAL unit's payload (emulation prevention bytes already removed).
class BitReader {
 public:
  void init(const uint8_t* data, uint64_t bytes);
  uint32_t bit() {
    if (pos_ >= size_) {
      error_ = true;
      return 0;
    }
    const uint32_t b = (data_[pos_ >> 3] >> (7 - (pos_ & 7))) & 1;
    ++pos_;
    return b;
  }
  uint32_t u(int n) {  // n <= 32
    uint32_t v = 0;
    if (pos_ + static_cast<uint64_t>(n) > size_) {
      error_ = true;
      pos_ = size_;
      return 0;
    }
    while (n > 0) {
      const int avail = 8 - static_cast<int>(pos_ & 7);
      const int take = n < avail ? n : avail;
      const uint32_t byte = data_[pos_ >> 3];
      const uint32_t bits = (byte >> (avail - take)) & ((1u << take) - 1);
      v = take == 32 ? bits : (v << take) | bits;
      pos_ += static_cast<uint64_t>(take);
      n -= take;
    }
    return v;
  }
  uint32_t ue();
  int32_t se() {
    const uint32_t k = ue();
    return (k & 1) ? static_cast<int32_t>((k + 1) >> 1) : -static_cast<int32_t>(k >> 1);
  }
  bool more_rbsp_data() const { return pos_ < stop_; }
  void align() { pos_ = (pos_ + 7) & ~uint64_t{7}; }
  uint64_t position() const { return pos_; }
  void seek(uint64_t pos) { pos_ = pos > size_ ? size_ : pos; }
  uint64_t bits_left() const { return size_ - pos_; }
  const uint8_t* data() const { return data_; }
  bool error() const { return error_; }

 private:
  const uint8_t* data_ = nullptr;
  uint64_t size_ = 0;  // in bits
  uint64_t stop_ = 0;  // position of the rbsp_stop_one_bit
  uint64_t pos_ = 0;
  bool error_ = false;
};

struct VlcEntry {
  uint8_t len;
  uint16_t code;
  uint8_t sym;
};

// A prefix code as a binary tree: node[i][bit] >= 0 is the next node, < 0 is a leaf holding symbol ~value.
struct VlcTree {
  int16_t node[128][2];
  int count = 0;
  void build(const VlcEntry* entries, int n);
  int decode(BitReader& br) const;  // symbol, or -1 on a code that does not exist
};

struct Sps {
  bool valid = false;
  int profile_idc = 0, level_idc = 0, id = 0;
  int chroma_format_idc = 1;
  bool qpprime_bypass = false;
  bool scaling_matrix_present = false;
  uint8_t list4[6][16];  // raster order
  uint8_t list8[2][64];
  int log2_max_frame_num = 4, poc_type = 0, log2_max_poc_lsb = 4;
  bool delta_pic_order_always_zero = false;
  int offset_for_non_ref_pic = 0, offset_for_top_to_bottom = 0, num_ref_in_cycle = 0;
  int offset_for_ref_frame[256];
  int max_num_ref_frames = 1;
  bool gaps_allowed = false;
  int mb_w = 0, mb_h = 0;  // in macroblocks (frame height)
  bool frame_mbs_only = true, mbaff = false, direct_8x8_inference = false;
  int crop_l = 0, crop_r = 0, crop_t = 0, crop_b = 0;  // in luma samples
  bool full_range = false;
  int num_reorder_frames = -1, max_dec_frame_buffering = -1;
  int dpb_frames = 1;  // how many frames the level allows
};

struct Pps {
  bool valid = false;
  int id = 0, sps_id = 0;
  bool cabac = false, bottom_field_pic_order = false;
  int num_slice_groups = 1;
  int num_ref_idx_default[2] = {1, 1};
  bool weighted_pred = false;
  int weighted_bipred_idc = 0;
  int pic_init_qp = 26, pic_init_qs = 26;
  int chroma_qp_offset[2] = {0, 0};
  bool deblocking_control = false, constrained_intra_pred = false, redundant_pic_cnt_present = false;
  bool transform_8x8 = false;
  bool scaling_matrix_present = false;
  uint8_t list4[6][16];
  uint8_t list8[2][64];
  bool list_present_any = false;
};

enum SliceType { kSliceP = 0, kSliceB = 1, kSliceI = 2 };

struct RefMod {
  int idc;
  uint32_t value;
};

struct Mmco {
  int op;
  uint32_t a, b;
};

struct SliceHeader {
  int nal_unit_type = 0, nal_ref_idc = 0;
  int first_mb = 0;
  int type = kSliceI;
  int pps_id = 0;
  int frame_num = 0;
  bool field_pic = false, bottom_field = false;
  int idr_pic_id = 0;
  int poc_lsb = 0, delta_poc_bottom = 0, delta_poc[2] = {0, 0};
  int redundant_pic_cnt = 0;
  bool direct_spatial_mv_pred = true;
  bool num_ref_override = false;
  int num_ref_idx[2] = {0, 0};
  int num_mod[2] = {0, 0};
  RefMod mods[2][kMaxRefs + 1];
  int luma_log2_denom = 0, chroma_log2_denom = 0;
  bool luma_weight_flag[2][kMaxRefs], chroma_weight_flag[2][kMaxRefs];
  int luma_weight[2][kMaxRefs], luma_offset[2][kMaxRefs];
  int chroma_weight[2][kMaxRefs][2], chroma_offset[2][kMaxRefs][2];
  bool no_output_of_prior_pics = false, long_term_reference = false, adaptive_marking = false;
  int num_mmco = 0;
  Mmco mmco[66];
  int cabac_init_idc = 0;
  int slice_qp = 26;
  int disable_deblocking = 0, alpha_off = 0, beta_off = 0;
  bool idr() const { return nal_unit_type == 5; }
};

// What later pictures need to know of a coded picture's motion (direct prediction) and the loop filter of this one.
struct MotionMb {
  int16_t mv[2][16][2];  // per 4x4 block, raster order
  int8_t ref[2][4];      // per 8x8 block, -1 if the list is not used
  int32_t refid[2][4];   // the picture each reference index named (Picture::id), -1 if none
  uint8_t intra;
};

// What the decoded picture buffer needs to know of a picture, apart from its samples: order, reference status and output state. The decoder's pictures
// (Picture) add the samples; a hardware path adds its own buffers beside it.
struct PictureInfo {
  int slot = 0;              // index in the buffer, set by Dpb::attach
  int id = 0;
  int poc = 0, frame_num = 0, frame_num_wrap = 0, long_term_idx = 0;
  int top_poc = 0, bottom_poc = 0;  // the order counts of the two fields of the frame (poc is the smaller)
  bool in_use = false;       // the slot holds a picture that is a reference or waits for output (or is being decoded)
  bool is_ref = false;       // used for (short- or long-term) reference
  bool is_long = false;
  bool needed_for_output = false;
  bool non_existing = false;
  bool keyframe = false;
  bool has_memory = false;   // the owner has samples/buffers in this slot already (a slot with memory is reused first)
  int64_t pts = 0;
};

struct Picture : PictureInfo {
  Mem<uint8_t> y, u, v;
  Mem<MotionMb> motion;
  int stride = 0, cstride = 0;
};

// The decoded picture buffer (8.2): picture order counts, reference marking and the reference lists of each slice, and the order pictures are output in. It
// works on the PictureInfo of a fixed set of slots the owner provides, so the software decoder (whose slots hold samples) and a hardware decoder (whose
// slots hold buffers in the GPU's memory) share the same rules.
class Dpb {
 public:
  // The slots (at most kMaxPictures) the buffer works on. Call again to start over with a different set.
  void attach(PictureInfo* const* slots, int count);
  void set_sps(const Sps* sps) { sps_ = sps; }

  // A new sequence size or a restart: every slot is empty, nothing is waiting for output.
  void clear_pictures();
  // After a seek: nothing is kept, order counting restarts.
  void reset();

  // Starts the picture of `sh` (its first slice): flushes at an IDR, computes the order count, picks a slot. Null if no slot can be freed. The caller allocates
  // the slot's memory and sets `has_memory`, or calls abort_picture().
  PictureInfo* begin_picture(const SliceHeader& sh, int64_t pts);
  void abort_picture();
  // The picture is decoded: marks references (8.2.5), updates the counters for the next picture, queues it for output.
  void finish_picture(const SliceHeader& last_slice);
  // The reference lists (8.2.4) for a slice of the current picture. Entries without a picture repeat the first one.
  void build_ref_lists(const SliceHeader& sh);

  // Output, in display order.
  void bump_output(bool all);
  bool output_ready() const { return out_count_ > 0; }
  PictureInfo* take_output();       // the next picture; it stays in use until the next call that releases it
  void release_returned() { returned_ = nullptr; }

  PictureInfo* cur() const { return cur_; }
  PictureInfo* ref(int list, int index) const { return ref_list_[list][index]; }
  int ref_count(int list) const { return ref_count_[list]; }
  int pic_poc() const { return pic_poc_; }
  int top_poc() const { return top_poc_; }
  int bottom_poc() const { return bottom_poc_; }

 private:
  PictureInfo* find_free_slot() const;
  PictureInfo* shorts_any_or_cur() const;
  void compute_poc(const SliceHeader& sh);
  void mark_references(const SliceHeader& sh);
  void sliding_window();
  void apply_mmco(const SliceHeader& sh);
  void queue_output(PictureInfo* p);

  PictureInfo* slots_[kMaxPictures] = {};
  int slot_count_ = 0;
  const Sps* sps_ = nullptr;
  PictureInfo* cur_ = nullptr;
  PictureInfo* returned_ = nullptr;   // the picture last handed out (not reused until the next call)
  PictureInfo* ref_list_[2][kMaxRefs + 1] = {};
  int ref_count_[2] = {0, 0};
  PictureInfo* out_wait_[kMaxPictures] = {};   // decoded, waiting for the display order to allow them out
  int out_wait_count_ = 0;
  PictureInfo* out_queue_[kMaxPictures] = {};  // ready for take_output()
  int out_count_ = 0;
  int next_pic_id_ = 1;
  int prev_poc_msb_ = 0, prev_poc_lsb_ = 0, cur_poc_msb_ = 0;
  int prev_frame_num_offset_ = 0, prev_frame_num_ = 0, prev_ref_frame_num_ = 0, frame_num_offset_ = 0;
  int top_poc_ = 0, bottom_poc_ = 0, pic_poc_ = 0;
  int max_long_term_idx_ = -1;
};

// The macroblock-level state kept for the picture being decoded (neighbour contexts and the loop filter).
struct MbCtx {
  int32_t slice;  // number of the slice that decoded it, -1 if not decoded yet
  uint8_t intra, skip, direct16, t8x8, i16, pcm;
  uint8_t cbp;           // luma in bits 0..3, chroma in bits 4..5
  uint8_t chroma_mode;
  int8_t qp, qpc[2];
  uint16_t nzmask;       // 4x4 luma blocks (raster, bit y*4+x) holding coefficients, for the loop filter
  uint8_t nz[16 + 4 + 4];  // coefficient counts: luma raster 4x4, Cb 2x2, Cr 2x2
  uint8_t cbf_dc;        // bit 0: Intra16x16 DC, 1: Cb DC, 2: Cr DC
  uint8_t direct_mask;   // 8x8 blocks predicted by direct mode (bit per 8x8)
  int8_t ipred[16];      // intra 4x4 / 8x8 prediction modes, raster 4x4
  uint8_t mvd[2][16][2];
  uint8_t dbf_idc;
  int8_t alpha_off, beta_off;
};

// The sequence and picture parameter sets seen so far, and the parsing of them and of slice headers (which depend on them). Shared by the
// decoder (Core) and by H264StreamParser, which hands the parsed syntax to a hardware decode engine.
class ParameterSets {
 public:
  Sps sps[32];
  Pps pps[256];
  uint8_t raw_present[256] = {};  // per PPS: which scaling lists it sent (bit per list)
  uint8_t raw_default[256] = {};  // per PPS: which of those say "use the default"

  bool parse_sps(BitReader& br);
  bool parse_pps(BitReader& br);
  bool parse_slice_header(BitReader& br, SliceHeader* sh) const;
  // The six 4x4 and two 8x8 scaling lists (raster order) a picture using `pps` is decoded with.
  void effective_scaling(const Sps& sps, const Pps& pps, uint8_t w4[6][16], uint8_t w8[2][64]) const;
  // The same lists in the order the bitstream sends them (zigzag scan), which is what a hardware decode engine is handed; effective_scaling() gives raster order.
  static void scaling_to_bitstream_order(const uint8_t w4[6][16], const uint8_t w8[2][64], uint8_t z4[6][16], uint8_t z8[2][64]);
  // 7.4.1.2.4: does slice `b` start a new picture after slice `a`? (`sps` is the active sequence parameter set.)
  static bool new_picture(const Sps& sps, const SliceHeader& a, const SliceHeader& b);
};

// Removes the emulation prevention bytes of a NAL unit (not its header byte) into `out`, which must hold `size` bytes. Returns the length.
uint64_t remove_emulation_prevention(const uint8_t* nal, uint64_t size, uint8_t* out);

// Calls `visit(const uint8_t* nal, uint64_t size)` for every NAL unit in `data`: length-prefixed (`nal_length_size` 1-4) or, when it is 0, Annex B.
template <class F>
void for_each_nal(ConstByteView data, int nal_length_size, F&& visit) {
  const uint8_t* d = data.data();
  const uint64_t n = data.size();
  if (nal_length_size > 0) {
    uint64_t pos = 0;
    while (pos + static_cast<uint64_t>(nal_length_size) <= n) {
      uint64_t len = 0;
      for (int i = 0; i < nal_length_size; ++i) len = (len << 8) | d[pos + static_cast<uint64_t>(i)];
      pos += static_cast<uint64_t>(nal_length_size);
      if (len == 0 || len > n - pos) break;
      visit(d + pos, len);
      pos += len;
    }
    return;
  }
  auto find_start = [&](uint64_t from) -> uint64_t {
    for (uint64_t k = from; k + 3 <= n; ++k)
      if (d[k] == 0 && d[k + 1] == 0 && d[k + 2] == 1) return k;
    return n;
  };
  uint64_t i = find_start(0);
  while (i < n) {
    const uint64_t begin = i + 3;
    const uint64_t next = find_start(begin);
    uint64_t end = next;
    while (end > begin && d[end - 1] == 0) --end;  // trailing_zero_8bits and the zero of a four-byte start code
    if (end > begin) visit(d + begin, end - begin);
    i = next;
  }
}

class Core {
 public:
  explicit Core(allocators::IAllocator& allocator) : allocator_(allocator) {
    PictureInfo* slots[kMaxPictures];
    for (int i = 0; i < kMaxPictures; ++i) slots[i] = &pics_[i];
    dpb_.attach(slots, kMaxPictures);
  }
  ~Core();

  bool configure(ConstByteView avcc);
  bool decode(ConstByteView data, int64_t pts_us);
  bool receive(VideoFrame* frame);
  void flush();
  void reset();
  VideoFormat output_format() const;
  bool ready() const { return dpb_.output_ready(); }
  void set_slice_observer(H264Decoder::SliceObserver observer, void* context) {
    observer_ = observer;
    observer_context_ = context;
  }

 private:
  // --- H264Syntax.cpp
  bool handle_nal(const uint8_t* nal, uint64_t size, int64_t pts);
  bool new_picture(const SliceHeader& a, const SliceHeader& b) const;
  void build_ref_lists(const SliceHeader& sh);  // the Dpb's lists, as pictures, and the implicit weights
  bool start_picture(const SliceHeader& sh, int64_t pts);
  void finish_picture();
  bool allocate_pictures();
  bool alloc_picture(Picture* p);
  void compute_scaling();
  void build_vlc();

  // --- H264Macroblock.cpp
  bool decode_slice(BitReader& br, const SliceHeader& sh);
  bool decode_mb_cavlc(BitReader& br, bool* is_end);
  bool parse_inter_cavlc_type(BitReader& br, int t0, bool* no_sub_lt8);
  bool finish_inter_parse(BitReader* br, int part_mode, const int* preds, const int* sub_shape, int refs[2][4]);
  void add_rect(int x, int y, int w, int h);
  bool read_residual_cavlc(BitReader& br);
  int cavlc_residual(BitReader& br, int nc, int max_coeff, int* lvl);
  int cavlc_nc(int bx, int by, int plane) const;
  void store4x4(int32_t* dst, const int* lvl, int start, int count, int list, int qp);
  void store8x8(int32_t* dst, const int* lvl, int list, int qp);
  void finish_luma_dc(const int* lvl);
  void finish_chroma_dc(int plane, const int* lvl);
  void update_qp();
  void set_inter_defaults();
  bool finish_mb(bool skipped);
  bool read_pcm(BitReader* br);
  int predicted_intra_mode(int bx, int by, bool is8x8) const;
  void fetch_motion(int list, int bx, int by, int* ref, int16_t* mv, bool unused = false) const;
  void predict_mv(int list, int bx, int by, int w, int h, int ref, int shape, int part, int* mvp);
  void set_motion(int list, int bx, int by, int w, int h, int ref, int mvx, int mvy);
  void set_mvd(int list, int bx, int by, int w, int h, int dx, int dy);
  void col_motion(int cx, int cy, bool* intra, int* refidx, int* refid, int16_t* mv) const;
  void direct_spatial(int blk8_mask);
  void direct_temporal(int blk8_mask);
  void direct_block(int blk8_mask);
  void skip_motion();
  const MbCtx* nb(int dx, int dy) const;
  bool intra_nb_ok(int dx, int dy) const;

  // --- H264Cabac.cpp
  bool decode_mb_cabac();
  bool read_residual_cabac();
  int cabac_residual(int cat, int max_coeff, int* lvl);
  bool cabac_cbf(int cat, int bx, int by, int plane);
  int cabac_decision(int ctx);
  int cabac_bypass();
  int cabac_terminate();
  void cabac_init_engine();
  void cabac_init_contexts();
  int cabac_mb_type_i(int base, bool in_pb);
  int cabac_mb_skip();
  int cabac_ref_idx(int list, int bx, int by);
  int cabac_mvd(int list, int comp, int bx, int by);
  int cabac_cbp();
  int cabac_qp_delta();
  int cabac_intra_chroma_mode();
  bool cabac_transform8x8_flag();

  // --- H264Predict.cpp
  void reconstruct_mb();
  void predict_intra4x4(int bx, int by, int mode);
  void predict_intra8x8(int b8, int mode);
  void predict_intra16(int mode);
  void predict_chroma(int mode);
  void residual_luma4x4(int bx, int by);
  void residual_luma8x8(int b8);
  void residual_chroma();
  void inter_predict();
  void mc_partition(int bx, int by, int w, int h);
  void idct4x4_add(uint8_t* dst, int stride, int32_t* c);
  void idct8x8_add(uint8_t* dst, int stride, int32_t* c);

  // --- H264Deblock.cpp
  void deblock_picture();
  void deblock_mb(int mbx, int mby);
  int boundary_strength(int mb_p, int mb_q, int blk_p, int blk_q, bool mb_edge) const;

  allocators::IAllocator& allocator_;

  ParameterSets ps_;
  const Sps* sp_ = nullptr;   // the active sequence parameter set
  const Pps* pp_ = nullptr;
  int nal_length_size_ = 0;   // 0: Annex B
  int active_sps_id_ = -1;
  Mem<uint8_t> rbsp_;

  Picture pics_[kMaxPictures];
  Dpb dpb_;                       // order counts, reference marking and lists, output order
  // Cached from dpb_ for the macroblock code: the picture being decoded and the reference lists of the current slice.
  Picture* cur_ = nullptr;
  Picture* ref_list_[2][kMaxRefs + 1];
  int ref_count_[2] = {0, 0};
  int pic_poc_ = 0;
  int mb_w_ = 0, mb_h_ = 0;
  Mem<MbCtx> mbs_;

  bool have_slice_ = false;
  SliceHeader prev_sh_;
  SliceHeader sh_;
  int slice_no_ = 0;
  int64_t pic_pts_ = 0;
  bool waiting_for_idr_ = true;
  H264Decoder::SliceObserver observer_ = nullptr;
  void* observer_context_ = nullptr;

  // LevelScale[list][qp % 6][position] for 4x4 (six lists) and 8x8 (intra and inter luma)
  int32_t level_scale4_[6][6][16];
  int32_t level_scale8_[2][6][64];

  // the macroblock being decoded
  int mbx_ = 0, mby_ = 0, mb_addr_ = 0;
  MbCtx* cm_ = nullptr;
  MotionMb* cmotion_ = nullptr;
  int mb_type_ = 0;     // MbKind in the .cpp files
  int part_mode_ = 0;   // 0 16x16, 1 16x8, 2 8x16, 3 8x8
  int i16_mode_ = 0;
  int cbp_ = 0;
  int qp_ = 0;
  int prev_qp_delta_nz_ = 0;
  uint16_t done_mask_ = 0;   // 4x4 blocks of the macroblock whose motion is set
  int32_t coef_[3][256];     // dequantised coefficients: luma (16 blocks of 16 in raster order, or 4 of 64), Cb, Cr (4 of 16)
  int32_t dc_luma_[16];
  bool coef_present_[3][16];
  uint8_t rect_[16][4];      // partitions for motion compensation: x, y, w, h in 4x4 units
  int rect_count_ = 0;

  // CABAC engine
  BitReader* cbr_ = nullptr;
  uint32_t c_range_ = 0, c_offset_ = 0;
  uint8_t c_state_[1024];

  VlcTree coeff_token_[4], coeff_token_chroma_dc_;
  VlcTree total_zeros_[15], total_zeros_chroma_dc_[3], run_before_[7];
  bool vlc_ready_ = false;

  int implicit_w_[kMaxRefs][kMaxRefs];  // weight of the list-1 prediction for implicit bi-prediction
};



}  // namespace h264
}  // namespace hardware
