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

#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Hardware/VideoCodec/Software/Av1BitReader.hpp"
#include "Hardware/VideoCodec/Software/Av1Types.hpp"
#include "Utility/ArrayView.hpp"

// OBU, sequence header and frame header parsing of the AV1 decoder: a port of dav1d's obu.c (BSD-2-Clause, Copyright (c) VideoLAN and
// dav1d authors) with the validation steps dav1d does. The parser keeps what dav1d keeps in c->seq_hdr, c->frame_hdr and
// c->refs[i].p.p.frame_hdr; the picture buffers, CDFs, segmentation maps and motion vectors per reference slot belong to the caller.
//
// Typical use for one temporal unit (a packet of an IVF, WebM or MP4 file: OBUs in the low-overhead format, Section 5):
//
//   ConstByteView rest = packet;
//   while (!rest.empty()) {
//     uint64_t used = 0;
//     ObuEvent ev;
//     if (!parser.parse_obu(rest, used, ev)) break;          // corrupt: dav1d drops the rest of the packet as well
//     rest = rest.subview(used);
//     switch (ev.kind) {
//       case ObuEventKind::SequenceHeader:    if (ev.new_sequence) { drop every picture, CDF, segmap and mv buffer }  break;
//       case ObuEventKind::ShowExistingFrame: output the picture in slot ev.existing_frame_idx;
//                                             if (ev.refresh_all_refs) copy that slot's picture state to all 8 slots;  break;
//       case ObuEventKind::FrameReady:        decode *parser.frame_header() with parser.tiles(); then
//                                             parser.ref_update(hdr.refresh_frame_flags, hdr) (the header may have been
//                                             completed by the decoder: shear parameters of gmv[]);  break;
//       default: break;
//     }
//   }
//
// The tile data views point into the packet passed to parse_obu: the packet has to stay alive until the frame has been decoded.

namespace hardware {
namespace av1 {

inline bool is_key_or_intra(const FrameHeader& hdr) { return (hdr.frame_type & 1) == 0; }
inline bool is_inter_or_switch(const FrameHeader& hdr) { return (hdr.frame_type & 1) != 0; }

// (poc0 - poc1) wrapped to the range of order_hint_n_bits bits, signed (dav1d's get_poc_diff).
inline int get_poc_diff(int order_hint_n_bits, int poc0, int poc1) {
  if (!order_hint_n_bits) return 0;
  const int mask = 1 << (order_hint_n_bits - 1);
  const int diff = static_cast<int>(static_cast<unsigned>(poc0) - static_cast<unsigned>(poc1));
  return (diff & (mask - 1)) - (diff & mask);
}

// dav1d_default_wm_params: the identity warp.
WarpedMotionParams default_wm_params();

enum class ObuEventKind : uint8_t {
  None,               // nothing to do: padding, unknown OBU, dropped by the operating point, redundant header, an OBU_FRAME_HDR that must be followed by tile groups
  TemporalDelimiter,  // a new temporal unit starts
  SequenceHeader,     // sequence_header() was (re)loaded
  FrameHeader,        // frame_header() was parsed; tile group OBUs have to follow
  ShowExistingFrame,  // output the picture of slot existing_frame_idx
  TileGroup,          // a tile group was added, the frame is not complete yet
  FrameReady,         // the last tile arrived: decode frame_header() with tiles()
  ContentLightLevel,  // content_light_level() changed
  MasteringDisplay,   // mastering_display() changed
  ItuT35,             // a registered user data message: itut_t35_* fields of the event
};

struct ObuEvent {
  ObuEventKind kind = ObuEventKind::None;
  // SequenceHeader: the sequence differs from the previous one (or there was none): every reference slot was invalidated.
  bool new_sequence = false;
  // SequenceHeader: only operating_parameter_info differs from the previous header of the same sequence.
  bool op_params_changed = false;
  // ShowExistingFrame: the shown frame is a key frame, so the decoding process refreshes all 8 slots with it (the header side is done by the parser;
  // the caller has to do the same for its pictures, CDFs, segmentation maps; motion vector buffers of the other slots are dropped).
  bool refresh_all_refs = false;
  uint8_t existing_frame_idx = 0;
  // ItuT35
  uint8_t itut_t35_country_code = 0;
  uint8_t itut_t35_country_code_extension = 0;
  ConstByteView itut_t35_payload;
};

// One tile: the bytes of the tile (without the tile size field) and its position in the tile grid.
struct TileData {
  ConstByteView data;
  uint16_t tile_row = 0;
  uint16_t tile_col = 0;
};

enum class ObuError : uint8_t {
  None,
  Truncated,     // an OBU or header ran past its data, or a trailing bit is missing
  Syntax,        // a syntax element has an illegal value or a header contradicts itself
  NoSequence,    // a frame header before any sequence header, or tile data without a frame header
  BadReference,  // a reference slot is empty, does not match (size ratio, bit depth, layout, frame id) or is not showable
  TileOrder,     // tile groups out of order, or a tile size that does not fit
  FrameSize,     // larger than the frame size limit
  NoMemory,
};

class ObuParser {
 public:
  explicit ObuParser(allocators::IAllocator& allocator);
  ~ObuParser();
  ObuParser(const ObuParser&) = delete;
  ObuParser& operator=(const ObuParser&) = delete;

  // Dav1dSettings: strict_std_compliance (off by default) and frame_size_limit (width * height, 0 = none).
  void set_strict_std_compliance(bool strict) { strict_ = strict; }
  void set_frame_size_limit(uint32_t limit) { frame_size_limit_ = limit; }
  // Operating point to decode (default 0). Takes effect with the next sequence header; an index that is out of range selects 0.
  void set_operating_point(unsigned index) { operating_point_ = index; }

  // Parses the first OBU of `data` (header, optional extension, size field and payload; without a size field the OBU extends to the end of
  // `data`). `consumed` is the number of bytes the OBU took (also set for dropped OBUs). False on a corrupt OBU (see error()): the state of
  // the current frame is discarded, the caller should skip the rest of the packet.
  bool parse_obu(ConstByteView data, uint64_t& consumed, ObuEvent& event);

  // Stores `hdr` (and the sequence's layout and bit depth) in every reference slot whose bit is set in refresh_frame_flags.
  void ref_update(unsigned refresh_frame_flags, const FrameHeader& hdr);
  // Forgets everything (a decoder flush after seeking): slots, headers.
  void reset();

  ObuError error() const { return error_; }
  const SequenceHeader* sequence_header() const { return have_seq_ ? &seq_ : nullptr; }
  // The frame header being assembled or the one of the frame that is ready; null after the frame was handed over (the next frame header
  // OBU replaces it) and for show_existing_frame headers it is only used inside the event.
  const FrameHeader* frame_header() const { return have_frame_ ? &frame_ : nullptr; }
  // The tiles of the frame: valid after FrameReady, indexed by tile number (row * cols + col).
  ArrayView<const TileData> tiles() const { return tiles_.empty() ? ArrayView<const TileData>() : ArrayView<const TileData>(tiles_.first(n_tiles_)); }
  // The header stored in reference slot `index`, null if the slot is empty.
  const FrameHeader* ref_header(unsigned index) const { return index < NUM_REF_FRAMES && refs_[index].valid ? &refs_[index].hdr : nullptr; }
  bool ref_showable(unsigned index) const { return index < NUM_REF_FRAMES && refs_[index].valid && refs_[index].showable; }
  unsigned operating_point_idc() const { return operating_point_idc_; }
  // The highest spatial layer of the operating point (0 if the stream has no spatial layers).
  unsigned max_spatial_id() const { return max_spatial_id_; }
  const ContentLightLevel* content_light_level() const { return have_cll_ ? &cll_ : nullptr; }
  const MasteringDisplay* mastering_display() const { return have_mdcv_ ? &mdcv_ : nullptr; }

 private:
  struct RefSlot {
    FrameHeader hdr;
    bool valid = false;
    bool showable = false;
    PixelLayout layout = PIXEL_LAYOUT_I400;
    int bpc = 8;
  };

  bool fail(ObuError e) {
    error_ = e;
    return false;
  }
  bool parse_seq_hdr(BitReader& gb, SequenceHeader& hdr);
  bool read_frame_size(BitReader& gb, bool use_ref);
  bool parse_frame_hdr(BitReader& gb);
  bool parse_tile_group(BitReader& gb, ConstByteView obu_payload, ObuEvent& event);
  bool check_references();
  bool handle_show_existing(ObuEvent& event);
  bool parse_metadata(BitReader& gb, ObuEvent& event);
  void invalidate_refs();
  bool reserve_tiles(uint64_t count);

  allocators::IAllocator& allocator_;
  bool strict_ = false;
  uint32_t frame_size_limit_ = 0;
  unsigned operating_point_ = 0;
  unsigned operating_point_idc_ = 0;
  unsigned max_spatial_id_ = 0;
  ObuError error_ = ObuError::None;

  SequenceHeader seq_{};
  bool have_seq_ = false;
  FrameHeader frame_{};
  bool have_frame_ = false;  // frame_ holds a parsed header
  bool frame_open_ = false;  // dav1d's c->frame_hdr != NULL: a header was parsed and its frame is not complete yet
  ContentLightLevel cll_{};
  MasteringDisplay mdcv_{};
  bool have_cll_ = false;
  bool have_mdcv_ = false;

  ArrayView<TileData> tiles_;  // capacity, owned (allocator_)
  uint64_t n_tiles_ = 0;       // tiles stored so far for the current frame

  RefSlot refs_[NUM_REF_FRAMES];
};

}  // namespace av1
}  // namespace hardware
