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

// An AV1 encoder of single frames driven from outside, the way Direct3D 12's AV1 video encoder is (ID3D12VideoEncodeCommandList2::EncodeFrame with
// D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_CODEC_DATA): the program keeps the eight reference slots (the reconstructed pictures), names the frame's type, order hint,
// references (ref_frame_idx), refresh flags and filters, and writes every OBU itself - the sequence header, the frame header and the tile group headers - from what it
// asked for and from the values the encoder reports after coding (D3D12_VIDEO_ENCODER_AV1_POST_ENCODE_VALUES: the quantizer, the primary reference frame, ...). The
// encoder writes the tiles' entropy-coded data, one after the other (each tile's size is reported: a tile group OBU gives every tile but the last its tile_size_minus_1),
// and the reconstruction (after the loop filter and CDEF), which is what a decoder shows and what later frames refer to.
//
// The tile data decode under headers of this form (the program's must say the same; write_sequence_header / write_frame_obu below write them):
//   sequence header: seq_profile 0 (Main: 4:2:0, 8 or 10 bits), still_picture 0, reduced_still_picture_header 0, no timing / decoder model info, one operating point,
//       frame_width_bits / frame_height_bits as needed, frame_id_numbers_present_flag 0, use_128x128_superblock 0, enable_filter_intra 0, enable_intra_edge_filter as
//       given, enable_interintra_compound 0, enable_masked_compound 0, enable_warped_motion 0, enable_dual_filter 0, enable_order_hint as given (order_hint_bits),
//       enable_jnt_comp 0, enable_ref_frame_mvs 0, seq_choose_screen_content_tools 0 with seq_force_screen_content_tools 0, enable_superres 0, enable_cdef as given,
//       enable_restoration 0, mono_chrome 0, color_description_present_flag 0, color_range 0, chroma_sample_position 0, separate_uv_delta_q 0, film_grain_params_present 0.
//   frame header: show_existing_frame 0, show_frame 1, frame_type as given, error_resilient_mode as given, disable_cdf_update as given, allow_screen_content_tools 0,
//       frame_size_override_flag 0 (a switch frame codes its size), order_hint, primary_ref_frame as reported (always PRIMARY_REF_NONE: the encoder keeps no CDFs
//       of earlier frames), refresh_frame_flags, ref_order_hint[] of the slots (error resilient inter frames), frame_refs_short_signaling 0, ref_frame_idx as given,
//       allow_high_precision_mv as given, the interpolation filter as given (is_filter_switchable 1 for Switchable), is_motion_mode_switchable 0,
//       use_ref_frame_mvs 0, disable_frame_end_update_cdf as given, the tile layout as reported, the quantizer as reported (base_q_idx, the DC / AC deltas as given,
//       using_qmatrix 0, delta_q_present / delta_q_res as reported), segmentation_enabled 0, delta_lf_present 0, the loop filter and CDEF parameters as given,
//       lr_type NONE, tx_mode_select (TX_MODE_LARGEST or TX_MODE_SELECT as given), reference_select 0, skip_mode_present 0, allow_warped_motion 0,
//       reduced_tx_set as given, every global motion identity, no film grain.
// What the tiles hold: 64x64 superblocks, square partitions (NONE / SPLIT, and what the frame edges force), intra blocks with the 13 luma modes at angle delta 0
// (the chroma mode DC or that of luma), inter blocks of a single reference (NEWMV / NEARESTMV / NEARMV / GLOBALMV, simple motion, the frame's filter or per block when
// switchable), DCT_DCT transforms of the largest size of the block (TX_MODE_SELECT: depth 0 / no split), the coefficients' full context coding, delta q per
// superblock for QP maps, cdef_idx per 64x64 block (chosen by the encoder).
#pragma once
#include "Allocators/IAllocator.hpp"
#include "Hardware/VideoCodec/Software/VideoEncPlanes.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

namespace av1 {
class FrameEncoder;
}

enum class Av1EncFrameType : uint8_t { Key = 0, Inter = 1, IntraOnly = 2, Switch = 3 };              // (D3D12_VIDEO_ENCODER_AV1_FRAME_TYPE's values)
enum class Av1EncTxMode : uint8_t { Only4x4 = 0, Largest = 1, Select = 2 };                          // (D3D12_VIDEO_ENCODER_AV1_TX_MODE's values; Only4x4 is lossless only: refused)
enum class Av1EncFilter : uint8_t { EightTap = 0, Smooth = 1, Sharp = 2, Bilinear = 3, Switchable = 4 };   // (D3D12_VIDEO_ENCODER_AV1_INTERPOLATION_FILTERS)
enum class Av1EncPrecision : uint8_t { Quarter, Half, Full };   // the finest vector the search may give (eighth sample vectors are never searched)
enum class Av1EncResult : uint8_t {
  Ok,
  PictureControlNotSupported,    // a frame description the encoder cannot code (type, flags, quantizer, filters out of range)
  SubregionLayoutNotSupported,   // the tile layout
  InvalidReferences,             // a reference slot the frame uses that is empty or of the wrong size
  NoMemory,
};

constexpr uint32_t kAv1EncMaxTileCols = 64, kAv1EncMaxTileRows = 64;
constexpr uint32_t kAv1EncSuperblock = 64;
constexpr uint8_t kAv1PrimaryRefNone = 7;

struct Av1EncSequence {
  uint32_t width = 0, height = 0;   // the frame size (and render size), 16 .. 8192 x 16 .. 8192 luma samples, even
  uint8_t bit_depth = 8;            // 8 or 10
  bool enable_order_hint = true;
  uint8_t order_hint_bits = 8;      // 1 .. 8 (OrderHintBitsMinus1 + 1)
  bool enable_cdef = false;
  bool enable_intra_edge_filter = false;
};

// A reference slot (ReferenceFramesReconPictureDescriptors[i]): the picture and what the frame header says of it.
struct Av1EncReference {
  bool valid = false;
  EncPlanes16 picture;
  uint32_t order_hint = 0;
  Av1EncFrameType frame_type = Av1EncFrameType::Key;
};

struct Av1EncLoopFilter {
  uint8_t level[4] = {0, 0, 0, 0};     // loop_filter_level[0] (vertical luma edges), [1] (horizontal luma), [2] U, [3] V; 0 .. 63
  uint8_t sharpness = 0;               // 0 .. 7
  bool delta_enabled = false, delta_update = false;
  int8_t ref_deltas[8] = {1, 0, 0, 0, -1, 0, -1, -1};   // -63 .. 63
  int8_t mode_deltas[2] = {0, 0};
};
struct Av1EncCdef {
  uint8_t damping_minus3 = 0;          // 0 .. 3
  uint8_t bits = 0;                    // 0 .. 3: 1 << bits strengths
  uint8_t y_pri[8] = {}, y_sec[8] = {}, uv_pri[8] = {}, uv_sec[8] = {};   // primary 0 .. 15, secondary 0 .. 3 (coded: 4 means 3 is not used, as the header has it)
};
// The tiles, in superblocks: `cols` columns of col_width_sb[i] and `rows` rows of row_height_sb[i] (filled by tile_layout for a uniform grid).
struct Av1EncTiles {
  bool uniform = true;                 // uniform_tile_spacing_flag (the sizes then follow from cols_log2 / rows_log2)
  uint32_t cols_log2 = 0, rows_log2 = 0;   // uniform only: TileColsLog2, TileRowsLog2
  uint32_t cols = 1, rows = 1;
  uint32_t col_width_sb[kAv1EncMaxTileCols] = {}, row_height_sb[kAv1EncMaxTileRows] = {};
  uint32_t context_update_tile_id = 0;
};

struct Av1EncPicture {
  Av1EncSequence seq;
  Av1EncFrameType type = Av1EncFrameType::Key;
  bool error_resilient = false, disable_cdf_update = false, disable_frame_end_update_cdf = false, allow_high_precision_mv = false, reduced_tx_set = false;
  Av1EncFilter filter = Av1EncFilter::EightTap;
  Av1EncTxMode tx_mode = Av1EncTxMode::Largest;
  uint32_t order_hint = 0;             // (written modulo 1 << order_hint_bits)
  uint8_t refresh_frame_flags = 0xFF;  // (a key frame refreshes every slot: 0xFF)
  Av1EncReference slots[8];
  uint8_t ref_frame_idx[7] = {};       // LAST .. ALTREF: the slot each refers to (inter and switch frames)
  uint8_t base_q_idx = 128;            // 1 .. 255 (0 is lossless: refused)
  int8_t delta_q_y_dc = 0, delta_q_u_dc = 0, delta_q_u_ac = 0, delta_q_v_dc = 0, delta_q_v_ac = 0;   // -64 .. 63
  ArrayView<const int16_t> qp_map;     // empty, or one value per 64x64 superblock (raster order): added to base_q_idx, or (qp_map_absolute) the superblock's qindex
  bool qp_map_absolute = false;
  Av1EncLoopFilter loop_filter;
  Av1EncCdef cdef;                     // (used only with seq.enable_cdef)
  Av1EncTiles tiles;
  Av1EncPrecision precision = Av1EncPrecision::Quarter;
};

// What the program writes into the frame header after coding (D3D12_VIDEO_ENCODER_AV1_POST_ENCODE_VALUES).
struct Av1EncPostEncode {
  uint8_t base_q_idx = 0;
  bool delta_q_present = false;
  uint8_t delta_q_res = 0;             // delta_q_res: the deltas are multiples of 1 << delta_q_res
  uint8_t primary_ref_frame = kAv1PrimaryRefNone;
  uint8_t ref_frame_idx[7] = {};
  Av1EncTiles tiles;                   // the layout coded (a uniform grid with its sizes filled in)
};

struct Av1EncTile {
  uint64_t offset = 0, size = 0;       // in the output
};

class Av1PictureEncoder {
 public:
  explicit Av1PictureEncoder(allocators::IAllocator& allocator);
  ~Av1PictureEncoder();
  Av1PictureEncoder(const Av1PictureEncoder&) = delete;
  Av1PictureEncoder& operator=(const Av1PictureEncoder&) = delete;

  // Completes a tile layout for a frame of width x height: a uniform grid of at least cols x rows tiles (cols_log2 / rows_log2 the smallest that give that many, the
  // sizes and the real counts as AV1's uniform spacing makes them), or checks explicit sizes (they must cover the frame's superblocks, every tile at most 4096 samples
  // wide and of at most 4096 * 2304 samples). False if the layout cannot be coded.
  [[nodiscard]] static bool tile_layout(uint32_t width, uint32_t height, Av1EncTiles* tiles);

  // Codes one frame: `source` and `reconstruction` are the sequence's size; the tiles are appended to `out` (one after the other) and described in `tiles` (cleared
  // first), the values the frame header must carry in *post.
  [[nodiscard]] Av1EncResult encode(const Av1EncPicture& picture, const EncPlanes16& source, const EncWritablePlanes16& reconstruction, ByteBuffer* out,
                                    List<Av1EncTile>* tiles, Av1EncPostEncode* post, EncStatistics* statistics);

  // The OBUs a program writes (what the header comment above lists), for tests and examples: a temporal delimiter and the sequence header OBU; a frame OBU (frame
  // header and one tile group of every tile, tile_start_and_end_present_flag 0, TileSizeBytes 4) of `tile_data` cut as `tiles` say. obu_has_size_field 1 throughout.
  [[nodiscard]] static bool write_sequence_header(const Av1EncSequence& seq, ByteBuffer* out);
  [[nodiscard]] static bool write_frame_obu(const Av1EncPicture& picture, const Av1EncPostEncode& post, ConstByteView tile_data, ArrayView<const Av1EncTile> tiles,
                                            ByteBuffer* out);

  // For tests: how many samples of the last frame the encoder's own reconstruction (before the in-loop filters) differs in from the decoder's, counted when the
  // frame has no loop filter and no CDEF (0 unless the encoder's prediction and the decoder's part ways).
  uint64_t last_drift() const;

 private:
  allocators::IAllocator& allocator_;
  UniquePointer<av1::FrameEncoder> core_;
};

}  // namespace hardware
