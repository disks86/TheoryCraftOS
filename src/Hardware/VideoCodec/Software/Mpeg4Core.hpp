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


// The internals of the MPEG-4 Part 2 / H.263 decoder: pictures, macroblocks and the Core class whose methods are spread over Mpeg4Syntax.cpp (packets, planes,
// video packets, macroblocks, blocks and the predictions of the bitstream: DC, AC and motion vectors) and Mpeg4Recon.cpp (inverse quantisation, the inverse
// transform, motion compensation). Section numbers refer to ISO/IEC 14496-2.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Hardware/VideoCodec/Software/Mpeg2Core.hpp"
#include "Hardware/VideoCodec/Software/Mpeg4.hpp"
#include "Hardware/VideoCodec/Software/Mpeg4Headers.hpp"
#include "Hardware/VideoCodec/VideoTypes.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {
namespace mpeg4 {

using h264::Mem;
using mpeg2::BitReader;   // reading past the end gives zeros and sets error()

constexpr int kPoolSize = 12;       // the two anchors, the picture being decoded, the held anchor, the pictures waiting for output, the one returned, one waiting for its time
constexpr int kOutputQueue = 6;
constexpr int kMaxMbWidth = 256, kMaxMbHeight = 256;   // 4096 x 4096

enum PictureType : int { kPictureI = 0, kPictureP = 1, kPictureB = 2 };

// What a B plane needs to know of the macroblocks of its backward reference (the colocated macroblock, 7.6.9.5).
enum MbFlags : uint8_t { kMbSkipped = 1, kMb8x8 = 2, kMbInterlaced = 4, kMbIntra = 8 };
struct MbInfo {
  int16_t mv[4][2];           // of each 8x8 block (zero: intra, skipped); a field macroblock keeps the average of its two vectors here
  int16_t field_mv[2][2];     // a field macroblock: the vector of each field
  uint8_t flags;
  uint8_t field_select[2];
  uint8_t pad;
};

// A decoded picture: whole macroblocks (the edges of the motion compensation are there, as in FFmpeg).
struct Picture {
  Mem<uint8_t> plane[3];
  int stride[3] = {0, 0, 0};
  int width = 0, height = 0;  // luma
  Mem<MbInfo> mbs;
  int64_t pts = 0;
  int type = kPictureI;
  bool in_use = false;
  bool output_pending = false;
  ByteView view(int c) const { return ByteView(plane[c].data(), plane[c].size()); }
};

// The parameters of the plane being decoded, from a plane header (or an H.263 picture header).
struct PlaneParams {
  int type = kPictureI;
  bool short_header = false;
  int quant = 1;
  int fcode_forward = 1, fcode_backward = 1;
  bool no_rounding = false;
  int dc_threshold = 99;
  bool alternate_scan = false, top_field_first = false;
  bool continuous_presence = false;   // H.263: GOB headers carry GSBI
  // H.263 optional modes (see ShortHeader).
  bool long_vectors = false, umv_plus = false, obmc = false, aic = false, deblocking = false, slices = false, aiv = false, modified_quant = false;
  int first_mb = 0;
  // An S plane (GMC; decoded as a P plane whose macroblocks may use the global motion, mcsel): the warp, as FFmpeg derives it from the trajectory.
  bool gmc = false;
  int warping_points = 0;             // the effective number: 1 is a translation
  int accuracy = 0;                   // sprite_warping_accuracy
  int sprite_offset[2][2] = {};       // [luma / chroma][x / y]
  int sprite_delta[2][2] = {};
  int sprite_shift[2] = {};
};

enum MvType : int { kMv16x16 = 0, kMv8x8 = 1, kMvField = 2 };
enum Direction : int { kForward = 1, kBackward = 2 };

// A macroblock, parsed.
struct Mb {
  bool intra = false, skipped = false, ac_pred = false, field_dct = false;
  bool mcsel = false;            // predicted with the global motion of an S plane
  bool aic_left = false;         // H.263 advanced intra coding with ac_pred: predicted from the left (else from above)
  int cbp = 0;                   // bit 5: block 0, ..., bit 0: block 5
  int mv_type = kMv16x16;
  int dir = kForward;            // kForward | kBackward
  int mv[2][4][2] = {};          // [direction][block or field][horizontal / vertical], in half or quarter samples
  int field_select[2][2] = {};   // [direction][field]
  int last[6] = {};              // the last coefficient of each block in scan order, -1: no coefficients
  int16_t block[6][64];          // by position (row * 8 + column)
};

// A macroblock of a data-partitioned packet between its partitions: the modes, coded pattern, quantiser and intra DC differentials (the vectors go to the
// picture's MbInfo directly).
struct PartitionMb {
  bool intra = false, skipped = false, four = false, ac_pred = false, dquant = false, use_dc_vlc = false, mcsel = false;
  uint8_t cbpc = 0, cbp = 0, qscale = 1;
  int16_t dc[6] = {};
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

 private:
  // --- Mpeg4Syntax.cpp
  bool build_vlcs();
  bool decode_packet(ConstByteView data, int64_t pts);
  bool decode_mpeg4(ConstByteView data, int64_t pts);
  bool decode_h263(ConstByteView data, int64_t pts);
  bool decode_vop(ConstByteView body, int64_t pts);
  bool configure(int width, int height);
  bool begin_picture(int64_t pts);
  void end_picture();
  void output_picture(Picture* p);
  Picture* find_free_picture();
  void decode_macroblocks(BitReader& br);
  int recover(BitReader& br, int index);
  bool partitioned_packet(BitReader& br, int start, int* count);
  bool partition_dc(BitReader& br, PartitionMb& m);
  bool macroblock_mpeg4(BitReader& br, Mb& mb);
  bool macroblock_h263(BitReader& br, Mb& mb);
  bool block_mpeg4(BitReader& br, Mb& mb, int n, bool coded, bool intra, bool use_dc_vlc);
  bool block_h263(BitReader& br, Mb& mb, int n, bool coded, bool intra);
  int read_vlc(BitReader& br, const Mem<uint16_t>& lut, int max_bits);
  bool read_motion(BitReader& br, int prediction, int fcode, int* value);
  void predict_motion(int block, int* px, int* py) const;
  bool same_packet(int mb_x, int mb_y) const;
  int predict_dc(int n, int level, int* direction);
  void predict_ac(Mb& mb, int n, int direction);
  void set_direct(Mb& mb, int mx, int my);
  int resync_prefix_length() const;
  int check_resync(BitReader& br);
  bool video_packet_header(BitReader& br, int* mb_index);
  bool gob_header(BitReader& br, int* mb_index);
  void record_macroblock(const Mb& mb);
  bool setup_sprite(const VopHeader& h);
  int chroma_quant(int q) const;
  void h263_dquant(BitReader& br);
  bool h263_motion(BitReader& br, int prediction, int* value);
  bool h263_vectors(BitReader& br, Mb& mb, bool four);
  void h263_intra_prediction(Mb& mb, int n);
  void preview_next(const BitReader& br);
  void overlapped_motion(const Picture& ref);
  void deblock_macroblock(const Mb& mb);
  int global_vector(int component) const;
  void global_motion(const Picture& ref);
  // --- Mpeg4Recon.cpp
  void reconstruct(Mb& mb);
  void conceal(int from, int to);
  void motion(const Mb& mb, int dir, const Picture& ref, bool average);
  void dequantize(Mb& mb, int n);

  allocators::IAllocator& allocator_;
  Vol vol_;
  bool have_vol_ = false;
  bool short_header_ = false;            // the stream is H.263 (picture start codes)
  uint8_t profile_and_level_ = 0;
  int width_ = 0, height_ = 0;           // as coded
  int mb_w_ = 0, mb_h_ = 0, mb_count_ = 0;
  bool low_delay_ = true;                // no B planes: pictures come out in decode order
  VideoFormat out_format_;

  Picture pool_[kPoolSize];
  Picture* cur_ = nullptr;
  Picture* anchor_[2] = {nullptr, nullptr};   // the older and the newer I or P picture
  Picture* held_ = nullptr;              // an anchor waiting for the next anchor before it is output
  Picture* queue_[kOutputQueue] = {};
  int queue_count_ = 0;
  Picture* returned_ = nullptr;
  bool need_intra_ = true;
  // A packed bitstream (DivX: an anchor and the B plane after it in one packet, then a placeholder packet): the B picture waits for the next packet and
  // takes its time, so that each picture carries the time of the packet that stands for it, as an unpacked stream's would.
  Picture* awaiting_time_ = nullptr;
  bool placeholder_expected_ = false;
  int pictures_in_packet_ = 0;

  // 6.3.5 / 7.6.9.5: the plane times (in ticks of the layer's time base) and the temporal distances of direct mode, kept as FFmpeg keeps them.
  int64_t time_base_ = 0, last_time_base_ = 0, time_ = 0, last_non_b_time_ = 0;
  int64_t pp_time_ = 0, pb_time_ = 0, pp_field_time_ = 0, pb_field_time_ = 0, t_frame_ = 0;

  // The plane being decoded.
  PlaneParams pic_;
  int qscale_ = 1;
  int packet_start_ = 0;                 // the first macroblock of the video packet (H.263: the group of blocks) being decoded
  int mb_x_ = 0, mb_y_ = 0;
  int last_mv_[2][2][2] = {};            // B planes: [direction][field][component], the predictors
  Mem<int32_t> mb_packet_;               // the video packet each macroblock of the plane was decoded in (-1: not yet)
  Mem<uint8_t> mb_intra_, mb_qscale_;
  Mem<int16_t> dc_[3], ac_[3];           // per block: the reconstructed DC, and the first row and column of quantised coefficients (16 per block)

  Mem<PartitionMb> partition_;           // data partitioning: the macroblocks of the packet being decoded
  bool dc_given_ = false;                // the texture of a partitioned intra macroblock: its DC differentials came in an earlier partition
  int16_t given_dc_[6] = {};
  bool rvlc_ = false;
  bool xvid_idct_ = false;               // the stream is Xvid's (its user data says so): its IDCT, as FFmpeg picks it                    // the texture is coded with reversible codes
  Mem<uint16_t> lut_rvlc_[2];
  Mem<uint16_t> lut_aic_;
  ShortHeader h263_;                     // the last H.263 picture header (a version 2 header may leave its modes out)
  bool have_h263_ = false;
  Mem<uint16_t> lut_intra_mcbpc_, lut_inter_mcbpc_, lut_cbpy_, lut_mv_, lut_dc_[2], lut_coef_[2], lut_btype_;
  uint8_t max_level_[2][2][64] = {};     // [intra / inter table][last][run]
  uint8_t max_run_[2][2][64] = {};       // [intra / inter table][last][level]
  uint8_t scan_tables_[3][64] = {};      // zigzag, alternate horizontal, alternate vertical
  uint8_t intra_matrix_[64] = {}, inter_matrix_[64] = {};   // the layer's quantiser matrices by position (row * 8 + column)
  bool vlcs_ready_ = false;
  Mb mb_;
};

}  // namespace mpeg4
}  // namespace hardware
