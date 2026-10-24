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


// The internals of the MPEG-2 decoder: headers, pictures and the Core class whose methods are spread over Mpeg2Syntax.cpp (start codes, headers, slices and
// macroblocks) and Mpeg2Recon.cpp (dequantization, the inverse transform and motion compensation). Section numbers refer to ISO/IEC 13818-2.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Hardware/VideoCodec/Software/H264Core.hpp"
#include "Hardware/VideoCodec/Software/Mpeg2.hpp"
#include "Hardware/VideoCodec/VideoTypes.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {
namespace mpeg2 {

using h264::Mem;

constexpr int kPoolSize = 10;       // the two anchors, the picture being decoded, the pictures waiting for output, and the spare
constexpr int kOutputQueue = 6;
constexpr int kMaxMbWidth = 256, kMaxMbHeight = 256;   // 4096 x 4096

enum PictureType : int { kPictureI = 1, kPictureP = 2, kPictureB = 3 };
enum PictureStructure : int { kTopField = 1, kBottomField = 2, kFramePicture = 3 };

// 6.2.2.1 and 6.2.2.3: the sequence header and its extensions.
struct SequenceHeader {
  bool valid = false;
  int horizontal_size = 0, vertical_size = 0;
  int aspect_ratio = 0, frame_rate_code = 0;
  int profile_and_level = 0;
  bool progressive_sequence = true, low_delay = false;
  int chroma_format = 1;
  uint8_t intra_matrix[64] = {};       // by position (row * 8 + column)
  uint8_t non_intra_matrix[64] = {};
  int mb_width = 0, mb_height = 0;     // of a frame, in macroblocks (a frame of an interlaced sequence is a whole number of macroblock pairs)
};

// 6.2.3 and 6.2.3.1: the picture header and the picture coding extension.
struct PictureHeader {
  int temporal_reference = 0;
  int type = kPictureI;
  int f_code[2][2] = {{15, 15}, {15, 15}};   // [forward / backward][horizontal / vertical]
  int intra_dc_precision = 0;                // 0 to 3: 8 to 11 bits
  int structure = kFramePicture;
  bool top_field_first = false, frame_pred_frame_dct = false, concealment_motion_vectors = false, q_scale_type = false, intra_vlc_format = false;
  bool alternate_scan = false, repeat_first_field = false, chroma_420_type = false, progressive_frame = true;
  bool has_extension = false;
};

// A decoded frame (both fields, or the one picture).
struct Picture {
  Mem<uint8_t> plane[3];
  int stride[3] = {0, 0, 0};
  int width = 0, height = 0;     // luma, whole macroblocks (the frame)
  int64_t pts = 0;
  int type = kPictureI;
  bool top_field_first = false, progressive_frame = true, repeat_first_field = false;
  bool in_use = false;           // not available for a new picture
  bool output_pending = false;   // in the output queue
  uint32_t order = 0;            // decoding order
};

// What the engine of an offload is given for each picture: the headers, the pictures it refers to (by slot: the index in the Core's pool), the slices.
struct OffloadPicture {
  const SequenceHeader* seq = nullptr;
  const PictureHeader* pic = nullptr;
  int slot = 0;
  int forward_slot = -1, backward_slot = -1;   // the pictures this one predicts from (-1: none)
  int anchor_slot[2] = {-1, -1};               // the older and the newer anchor picture when this one starts: the pictures an offload must still hold
  bool second_field = false;                   // a field picture whose frame was started by the first field
  ArrayView<const ConstByteView> slices;       // each slice with its start code (00 00 01 xx)
  uint8_t intra_matrix_scan[64] = {}, non_intra_matrix_scan[64] = {};   // the quantiser matrices in the order of the scan the picture uses (the i-th byte weighs the i-th coefficient)
  // The frame to fill (8-bit planes; for a field picture the whole frame, of which the offload writes its field's lines).
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

// 6.3: the bits of a start-code delimited unit, most significant first. Reading past the end gives zeros and sets error().
class BitReader {
 public:
  BitReader() = default;
  explicit BitReader(ConstByteView data) : data_(data) {}
  uint32_t peek(int n) const {   // n <= 24
    uint32_t v = 0;
    const uint64_t byte = pos_ >> 3;
    for (uint64_t i = 0; i < 4; ++i) v = (v << 8) | (byte + i < data_.size() ? data_[byte + i] : 0);
    return (v << (pos_ & 7)) >> (32 - n);
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
  void align() { pos_ = (pos_ + 7) & ~uint64_t{7}; }
  uint64_t bits_left() const { return pos_ >= data_.size() * 8 ? 0 : data_.size() * 8 - pos_; }
  bool error() const { return error_; }
  uint64_t position() const { return pos_; }

 private:
  ConstByteView data_;
  uint64_t pos_ = 0;
  bool error_ = false;
};

class Core {
 public:
  explicit Core(allocators::IAllocator& allocator);
  ~Core() = default;
  Core(const Core&) = delete;
  Core& operator=(const Core&) = delete;

  bool decode(ConstByteView data, int64_t pts_us);
  bool receive(VideoFrame* frame);
  void flush();
  void reset();
  VideoFormat output_format() const;
  bool ready() const { return queue_count_ > 0; }
  void set_offload(IOffload* offload) { offload_ = offload; offloaded_ = false; }
  void set_frame_observer(IFrameObserver* observer) { observer_ = observer; }

 private:
  // 6.3.17.1 frame_motion_type / field_motion_type, normalized
  enum MotionType { kMvFrame = 0, kMvField = 1, kMv16x8 = 2, kMvDmv = 3 };
  // What a macroblock is, parsed.
  struct Mb {
    bool intra = false, forward = false, backward = false, pattern = false, quant = false;
    bool skipped = false, field_dct = false;
    int motion = kMvFrame;
    int cbp = 0;
    int mv[2][4][2] = {};          // [direction][vector][horizontal / vertical] (vectors 2 and 3: the dual prime ones)
    int field_select[2][2] = {};   // [direction][vector]
  };
  // --- Mpeg2Syntax.cpp and Mpeg2Slice.cpp
  bool handle_unit(ConstByteView unit, int64_t pts);   // `unit` starts at the start code's 00 00 01
  bool parse_sequence_header(BitReader& br);
  bool parse_extension(BitReader& br);
  bool parse_picture_header(BitReader& br);
  bool start_picture();
  void finish_picture();
  bool decode_slice(ConstByteView unit, int mb_row);
  bool macroblock(BitReader& br, Mb& mb);
  bool decode_block(BitReader& br, Mb& mb, int block, int16_t* out);
  int read_motion_component(BitReader& br, int f_code, int prediction);
  bool read_motion_vectors(BitReader& br, Mb& mb);
  void output_picture(Picture* p);
  Picture* find_free_picture();
  bool build_vlcs();
  // --- Mpeg2Recon.cpp
  void reconstruct(Mb& mb, int mb_x, int mb_y);

  allocators::IAllocator& allocator_;
  SequenceHeader seq_, sequence_;     // seq_: the headers of the stream; sequence_: what the picture being decoded was started with
  PictureHeader pic_;
  bool have_sequence_ = false, have_picture_header_ = false, picture_ready_ = false;
  bool matrix_loaded_[2] = {false, false};

  Picture pool_[kPoolSize];
  Picture* cur_ = nullptr;            // the frame being decoded
  Picture* anchor_[2] = {nullptr, nullptr};   // older and newer I or P frame
  Picture* held_ = nullptr;           // an anchor frame waiting for the next anchor before it is output
  Picture* queue_[kOutputQueue] = {};
  int queue_count_ = 0;
  Picture* returned_ = nullptr;
  uint32_t decode_counter_ = 0;
  int64_t pts_ = 0;                   // of the picture being decoded
  bool first_field_done_ = false;     // cur_ has its first field; the second is expected
  bool second_field_ = false;         // the picture being decoded is the second field of cur_
  int first_field_structure_ = 0;
  bool picture_open_ = false;
  bool need_intra_ = true;            // decoding starts at an intra picture
  int slice_count_ = 0;
  List<ConstByteView> slices_;        // the slices of the picture being collected for the offload

  IOffload* offload_ = nullptr;
  IFrameObserver* observer_ = nullptr;
  bool offloaded_ = false;

  // slice and macroblock state
  int quant_scale_code_ = 1;
  int dc_pred_[3] = {0, 0, 0};
  int pmv_[2][2][2] = {};             // [forward / backward][first / second vector][horizontal / vertical]
  int mb_x_ = 0, mb_y_ = 0;
  VideoFormat out_format_;
  uint8_t vlc_pad_ = 0;
  Mem<uint16_t> lut_address_, lut_pattern_, lut_motion_, lut_dc_luma_, lut_dc_chroma_, lut_dct_[2];
  bool vlcs_ready_ = false;
  int16_t blocks_[6][64];
};

}  // namespace mpeg2
}  // namespace hardware
