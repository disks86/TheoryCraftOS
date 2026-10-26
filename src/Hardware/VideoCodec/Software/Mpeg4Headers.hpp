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
#include "Utility/ArrayView.hpp"

namespace hardware {
namespace mpeg4 {

// The headers of an MPEG-4 Part 2 (ISO/IEC 14496-2) elementary stream that a hardware decoder needs to be told about: the video object layer (the stream's parameters:
// size, time base, the tools in use, the quantiser matrices) and, for each video object plane, its coding type and whether it carries a picture. Everything below the
// plane header is the decoder's. Only what the Simple and Advanced Simple profiles use is understood - rectangular 4:2:0, 8-bit, no scalability, no sprites, no
// complexity estimation, no new prediction, no reduced resolution; anything else (and the H.263 short header) makes the parse fail, so a stream is taken whole or not at all.
// The parse follows the order of fields in the standard (and was checked against FFmpeg's decode_vol_header on streams from FFmpeg's encoder).

// The default quantiser matrices (the standard's tables, in raster order), and the zigzag scan that takes a raster index to the scan position. Matrices in a Vol are
// kept in SCAN order, as the bitstream sends them.
extern const uint8_t kDefaultIntraMatrix[64];
extern const uint8_t kDefaultNonIntraMatrix[64];
extern const uint8_t kZigzag[64];

struct Vol {
  uint32_t width = 0, height = 0;                   // luma samples, as coded (a multiple of 16 is not required)
  uint32_t time_increment_resolution = 0;           // ticks per second
  uint32_t time_increment_bits = 1;                 // bits of a plane's vop_time_increment
  uint8_t verid = 1;                                // video_object_layer_verid (1 when the layer carries no identifier)
  bool is_object_layer_identifier = false;
  bool fixed_vop_rate = false;
  bool interlaced = false;
  bool obmc_disable = false;
  bool quant_type_mpeg = false;                     // quant_type 1: the quantiser matrices apply (otherwise the H.263 quantiser)
  bool quarter_sample = false;
  bool resync_marker_disable = false;
  bool data_partitioned = false;
  bool reversible_vlc = false;
  bool low_delay = false;                           // no B planes: pictures come out in decode order
  uint8_t profile_and_level = 0;                    // from the visual object sequence header, 0 when there was none
  uint8_t intra_matrix[64] = {};                    // scan order
  uint8_t non_intra_matrix[64] = {};
};

enum class VopType : uint8_t { I = 0, P = 1, B = 2, S = 3 };

struct Vop {
  VopType type = VopType::I;
  bool coded = false;                               // vop_coded: false means "repeat the previous picture"
  uint32_t modulo_time_base = 0, time_increment = 0;
};

// What one packet (a run of start-code units) holds.
struct Packet {
  bool has_vol = false;
  Vol vol;
  uint32_t vop_count = 0;                           // video object planes in the packet (more than one: a packed bitstream)
  Vop vop;                                          // the first
};

// Reads the start codes in `data`. A video object layer header is parsed (into packet->vol); a plane header is parsed with the layer's time base, either the one just
// read or `current` (null: none known, so a plane cannot be read). Fails on a header that is cut short or uses a tool listed above. A packet with neither is a failure.
bool parse_packet(ConstByteView data, const Vol* current, Packet* packet);

}  // namespace mpeg4
}  // namespace hardware
