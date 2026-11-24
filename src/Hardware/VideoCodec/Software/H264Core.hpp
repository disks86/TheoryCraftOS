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
  ArrayView<T> view() const { return ArrayView<T>(p_, count_); }

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
  int chroma_format_idc = 1;  // 1 (4:2:0) or 0 (monochrome: the chroma planes are mid-grey)
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

// What later pictures need to know of a coded picture's motion (direct prediction) and the loop filter of this one. A frame keeps one per macroblock address
// (an MBAFF frame in its pair order); a frame decoded as two field pictures keeps the top field's macroblocks first, then the bottom field's.
struct MotionMb {
  int16_t mv[2][16][2];  // per 4x4 block, raster order (vertical in field units for a field macroblock)
  int8_t ref[2][4];      // per 8x8 block, -1 if the list is not used
  int32_t refid[2][4];   // the picture each reference index named: 4 * Picture::id + 1 (its top field), 2 (its bottom field) or 3 (the frame); -1 if none
  uint8_t intra;
  uint8_t field;         // a field macroblock (of a field picture, or mb_field_decoding_flag in an MBAFF frame)
};

// What the decoded picture buffer needs to know of a picture, apart from its samples: order, reference status and output state. The decoder's pictures
// (Picture) add the samples; a hardware path adds its own buffers beside it.
struct PictureInfo {
  int slot = 0;              // index in the buffer, set by Dpb::attach
  int id = 0;
  int poc = 0, frame_num = 0, frame_num_wrap = 0, long_term_idx = 0;
  int top_poc = 0, bottom_poc = 0;  // the order counts of the two fields of the frame (poc is the smaller of those decoded)
  bool in_use = false;       // the slot holds a picture that is a reference or waits for output (or is being decoded)
  bool is_ref = false;       // a field or both are used for (short- or long-term) reference (kept from field_ref)
  bool is_long = false;      // a field or both are used for long-term reference (kept from field_ref)
  uint8_t field_ref[2] = {0, 0};   // per field (top, bottom): 0 unused for reference, 1 short-term, 2 long-term
  uint8_t decoded = 0;       // the fields that hold decoded samples (bit 0 top, bit 1 bottom; 3 for a frame)
  bool field_coded = false;  // decoded as field pictures (a complementary field pair, or a field alone) rather than as a frame
  bool mbaff = false;        // decoded as an MBAFF frame
  bool needed_for_output = false;
  bool non_existing = false;
  bool keyframe = false;
  bool has_memory = false;   // the owner has samples/buffers in this slot already (a slot with memory is reused first)
  bool has_key = false;      // decoding for a DXVA driver: the slot holds the picture of surface `key`
  uint32_t key = 0;
  int64_t pts = 0;
};

struct Picture : PictureInfo {
  Mem<uint8_t> y, u, v;
  Mem<MotionMb> motion;
  int stride = 0, cstride = 0;
  // Decoding for a DXVA driver: a copy of the picture of surface `key` (the one with id `alias_of`), for a program that names that surface a second time with another
  // frame number or long-term status (ffmpeg gives an inferred "non-existing" frame the surface of the reference before it). has_key stays false for a copy.
  bool alias = false;
  int alias_of = 0;
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
  // the slot's memory and sets `has_memory`, or calls abort_picture(). The second field of a complementary field pair (7.4.1.2.4: the field after a first field of
  // the other parity with the same frame_num and the same reference status) goes into the slot of the first: that slot comes back again, with its memory.
  // `into` (decoding for a DXVA driver): the field goes into that slot, which holds the other field of its frame.
  PictureInfo* begin_picture(const SliceHeader& sh, int64_t pts, PictureInfo* into = nullptr);
  void abort_picture();
  // 8.2.5.2: does the picture `sh` starts (its first slice) leave a gap in frame_num, a frame_num that is neither PrevRefFrameNum nor the one after it? (Never for an
  // IDR picture, nor before the first reference picture of the sequence.)
  bool has_frame_num_gap(const SliceHeader& sh) const;
  // Called with each inferred "non-existing" frame once it has a slot: the owner gives the slot memory and sample values (any values: the standard leaves them open)
  // and returns false if it cannot. `previous` is the short-term reference decoded (or inferred) last before the frame, null if there is none; it is the frame's own
  // slot when the sliding window freed that one for it.
  using NonExistingHook = bool (*)(void* context, PictureInfo* frame, const PictureInfo* previous);
  // The decoding process for gaps in frame_num (8.2.5.2), before begin_picture() of the picture that has the gap: one "non-existing" frame for each missing value of
  // frame_num, in order, each a short-term reference after the sliding window (8.2.5.3) made room for it, never output. Their order counts come from 8.2.1 when
  // pic_order_cnt_type is 1 or 2 (which also carries FrameNumOffset on to the picture); with type 0 the standard derives none (the frames keep the previous
  // picture's counts and stay out of B slices' lists, 8.2.4.2.3). PrevRefFrameNum becomes the last inferred frame_num (7.4.3). Only the frames that can still be
  // references when the gap is over get slots (the sliding window of the later ones would drop the others again; the counters still step through them). False if
  // no slot could be freed or the hook failed.
  [[nodiscard]] bool fill_frame_num_gap(const SliceHeader& sh, NonExistingHook prepare, void* context);
  // Decoding for a DXVA driver: the order counts of the current picture are the program's (after begin_picture(); for a field only its own count is used).
  void set_current_poc(int top, int bottom);
  // Decoding for a DXVA driver: the next field pairs only with the frame begin_picture() is told to put it into, not with the field decoded before it.
  void forget_pending() { pending_ = nullptr; }
  // The structure of the current picture: 1 a top field, 2 a bottom field, 3 a frame; and whether it is the second field of its frame.
  int structure() const { return structure_; }
  bool second_field() const { return second_field_; }
  // Decoding for a DXVA driver: finished pictures are not queued for output; the last one stays untouched until the next call that releases it.
  void set_decode_order(bool on) { decode_order_ = on; }
  // A new picture identity (for a picture the owner puts in a slot itself).
  int new_id() { return next_pic_id_++; }
  // The picture is decoded: marks references (8.2.5), updates the counters for the next picture, queues it for output.
  void finish_picture(const SliceHeader& last_slice);
  // The reference lists (8.2.4) for a slice of the current picture. Entries without a picture repeat the first one. A field picture's entries are fields
  // (ref_parity() says which field of the frame), a frame's are frames.
  void build_ref_lists(const SliceHeader& sh);

  // Output, in display order. A first field still waiting for its pair goes out (unpaired) when `all` is set.
  void bump_output(bool all);
  bool output_ready() const { return out_count_ > 0; }
  PictureInfo* take_output();       // the next picture; it stays in use until the next call that releases it
  void release_returned() { returned_ = nullptr; }

  PictureInfo* cur() const { return cur_; }
  PictureInfo* ref(int list, int index) const { return ref_list_[list][index]; }
  int ref_parity(int list, int index) const { return ref_parity_[list][index]; }   // 0 the frame, 1 its top field, 2 its bottom field
  int ref_count(int list) const { return ref_count_[list]; }
  int pic_poc() const { return pic_poc_; }   // PicOrderCnt( CurrPic ): the field's count for a field
  int top_poc() const { return top_poc_; }
  int bottom_poc() const { return bottom_poc_; }

 private:
  PictureInfo* find_free_slot() const;
  PictureInfo* claim_slot();
  PictureInfo* shorts_any_or_cur() const;
  void compute_poc(const SliceHeader& sh);
  void mark_references(const SliceHeader& sh);
  void sliding_window();
  void apply_mmco(const SliceHeader& sh);
  void queue_output(PictureInfo* p);
  void release_pending();
  int entry_poc(const PictureInfo& p) const;
  int field_pic_num(const PictureInfo& p, int parity) const;

  PictureInfo* slots_[kMaxPictures] = {};
  int slot_count_ = 0;
  const Sps* sps_ = nullptr;
  PictureInfo* cur_ = nullptr;
  PictureInfo* returned_ = nullptr;   // the picture last handed out (not reused until the next call)
  PictureInfo* pending_ = nullptr;    // a first field whose frame waits for the second field
  int pending_structure_ = 0;
  bool pending_ref_ = false;
  int structure_ = 3;                 // of the current picture
  bool second_field_ = false;
  bool cur_long_ = false;             // memory_management_control_operation 6 marked the current picture long-term
  PictureInfo* ref_list_[2][kMaxRefs + 1] = {};
  uint8_t ref_parity_[2][kMaxRefs + 1] = {};
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
  bool decode_order_ = false;
  bool have_prev_ref_ = false;   // a reference picture was decoded since the sequence started (PrevRefFrameNum is defined)
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
  uint8_t field;         // a field macroblock (mb_field_decoding_flag; set for every macroblock of a field picture)
};

// A reference picture as the macroblocks of a slice use it: a frame (parity 0), or the top (1) or bottom (2) field of one.
struct RefPic {
  Picture* pic = nullptr;
  uint8_t parity = 0;
};

// A neighbouring location (6.4.12): the macroblock that holds it (-1: not available) and the location inside that macroblock.
struct NbLoc {
  int addr = -1;
  int x = 0, y = 0;
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
  H264Decoder::ExternalStatus decode_external(ConstByteView data, const H264Decoder::ExternalPicture& picture, VideoFrame* decoded, int* structure);
  void set_slice_observer(H264Decoder::SliceObserver observer, void* context) {
    observer_ = observer;
    observer_context_ = context;
  }

 private:
  // --- H264Syntax.cpp
  bool handle_nal(const uint8_t* nal, uint64_t size, int64_t pts);
  bool new_picture(const SliceHeader& a, const SliceHeader& b) const;
  void build_ref_lists(const SliceHeader& sh);  // the Dpb's lists, as pictures, the lists of field macroblocks and the implicit weights
  bool start_picture(const SliceHeader& sh, int64_t pts);
  void finish_picture();
  bool allocate_pictures();
  bool alloc_picture(Picture* p);
  static bool prepare_non_existing(void* context, PictureInfo* frame, const PictureInfo* previous);
  [[nodiscard]] bool copy_picture(Picture* to, const Picture& from);
  void make_grey(Picture* p);
  bool apply_external_references(const SliceHeader& sh);
  void compute_scaling();
  void build_vlc();
  void implicit_weights(int set, int cur_poc);

  // --- H264Macroblock.cpp
  bool decode_slice(BitReader& br, const SliceHeader& sh);
  bool decode_slice_cavlc(BitReader& br, int addr);
  bool decode_slice_cabac(int addr);
  void setup_mb(int addr);
  void set_mb_field(bool field);
  bool inferred_mb_field() const;
  bool decode_mb_cavlc(BitReader& br);
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
  void fetch_motion(int list, int xn, int yn, int* ref, int16_t* mv) const;
  void predict_mv(int list, int bx, int by, int w, int h, int ref, int shape, int part, int* mvp);
  void set_motion(int list, int bx, int by, int w, int h, int ref, int mvx, int mvy);
  void set_mvd(int list, int bx, int by, int w, int h, int dx, int dy);
  struct Colocated {
    bool intra = true;      // (also: no motion to use)
    int refidx = -1;        // refIdxCol
    int refid = -1;         // the picture refIdxCol named (MotionMb::refid)
    int16_t mv[2] = {0, 0}; // mvCol, unscaled
    int scale = 0;          // vertMvScale: 0 One_To_One, 1 Frm_To_Fld, 2 Fld_To_Frm
    bool field_mb = false;  // the co-located macroblock is a field macroblock
  };
  void col_motion(int bx, int by, Colocated* out) const;
  int map_col_to_list0(const Colocated& col) const;
  void direct_spatial(int blk8_mask);
  void direct_temporal(int blk8_mask);
  void direct_block(int blk8_mask);
  void skip_motion();
  NbLoc neighbour(int xn, int yn, bool chroma) const;
  const MbCtx* nb_mb(const NbLoc& n) const { return n.addr >= 0 ? &mbs_[static_cast<uint64_t>(n.addr)] : nullptr; }
  const MbCtx* nb_a() const;
  const MbCtx* nb_b() const;
  bool intra_ok(int xn, int yn, bool chroma) const;
  bool intra_rows_ok(int xn, int y0, int rows, bool chroma) const;
  const MotionMb& motion_of(int addr) const { return cur_->motion[static_cast<uint64_t>(motion_base_ + addr)]; }
  const RefPic& ref_pic(int list, int idx) const { return refs_[rl_][list][idx]; }

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
  int cabac_mb_field();
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
  void deblock_mb_mbaff(int addr);
  int boundary_strength(int mb_p, int mb_q, int blk_p, int blk_q, bool mb_edge, bool vertical) const;

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
  // The reference lists: [0] the slice's (frames, or fields in a field picture), [1] / [2] those of the top / bottom field macroblocks of an MBAFF frame (8.4.2.1:
  // entry 2i the field of frame i with the macroblock's parity, 2i + 1 the other one).
  RefPic refs_[3][2][2 * kMaxRefs + 1];
  int ref_n_[3][2] = {};
  int rl_ = 0;                    // the lists the current macroblock uses
  int ref_count_[2] = {0, 0};     // (of refs_[rl_])
  int pic_poc_ = 0;               // PicOrderCnt( CurrPic )
  int mb_w_ = 0, mb_h_ = 0;       // the frame, in macroblocks
  Mem<MbCtx> mbs_;                // per macroblock address of the current picture

  // The current picture: its structure, and the planes it is decoded into (a field's are every other line of its frame).
  int structure_ = 3;             // 1 top field, 2 bottom field, 3 frame
  bool field_pic_ = false, mbaff_ = false;
  bool mono_ = false;             // chroma_format_idc 0: no chroma syntax, the chroma planes stay mid-grey
  int pic_mb_h_ = 0;              // PicHeightInMbs (rows of the field for a field picture)
  int pic_mbs_ = 0;               // PicSizeInMbs
  int motion_base_ = 0;           // where the current picture's macroblocks are in Picture::motion
  uint8_t* pic_y_ = nullptr;
  uint8_t* pic_u_ = nullptr;
  uint8_t* pic_v_ = nullptr;
  int pic_ls_ = 0, pic_cs_ = 0;   // line steps of the current picture's planes

  bool have_slice_ = false;
  SliceHeader prev_sh_;
  SliceHeader sh_;
  int slice_no_ = 0;
  int64_t pic_pts_ = 0;
  bool waiting_for_idr_ = true;
  // Decoding for a DXVA driver (decode_external): the picture's description, and what happened while decoding it.
  const H264Decoder::ExternalPicture* ext_ = nullptr;
  bool ext_started_ = false, ext_missing_ = false, ext_no_memory_ = false;
  Picture* ext_decoded_ = nullptr;
  H264Decoder::SliceObserver observer_ = nullptr;
  void* observer_context_ = nullptr;

  // LevelScale[list][qp % 6][position] for 4x4 (six lists) and 8x8 (intra and inter luma)
  int32_t level_scale4_[6][6][16];
  int32_t level_scale8_[2][6][64];

  // the macroblock being decoded
  int mbx_ = 0, mby_ = 0, mb_addr_ = 0;   // mby_: the row in the macroblock's own grid (a field macroblock's is the row of its pair)
  bool mb_field_ = false;                 // a field macroblock (field picture, or mb_field_decoding_flag)
  bool mb_bottom_ = false;                // MBAFF: the bottom macroblock of its pair
  uint8_t* mb_y_ = nullptr;               // the macroblock's top-left samples
  uint8_t* mb_u_ = nullptr;
  uint8_t* mb_v_ = nullptr;
  int mb_ls_ = 0, mb_cs_ = 0;             // and the steps from one of its lines to the next
  int mc_y_ = 0, mc_h_ = 0;               // its top row and the height in the sample grid of its references (frame or field), luma
  int mb_poc_ = 0;                        // the order count of the picture (or, for a field macroblock of a frame, the field) it is in
  const uint8_t* scan4_ = nullptr;        // the inverse scans in use (8.5.6: zig-zag, or field scan for field macroblocks)
  const uint8_t* scan8_ = nullptr;
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
  bool bottom_skip_known_ = false, bottom_skipped_ = false;   // MBAFF CABAC: the bottom macroblock's mb_skip_flag, read with the top's

  // CABAC engine
  BitReader* cbr_ = nullptr;
  uint32_t c_range_ = 0, c_offset_ = 0;
  uint8_t c_state_[1024];

  VlcTree coeff_token_[4], coeff_token_chroma_dc_;
  VlcTree total_zeros_[15], total_zeros_chroma_dc_[3], run_before_[7];
  bool vlc_ready_ = false;

  // Weight of the list-1 prediction for implicit bi-prediction, per pair of reference indices, for each set of lists of refs_.
  int implicit_w_[3][2 * kMaxRefs][2 * kMaxRefs];
};



}  // namespace h264
}  // namespace hardware
